#include "markettwin/simulation/fork.hpp"

#include "markettwin/common/hash.hpp"
#include "markettwin/simulation/config.hpp"

#include "../telemetry/codec.hpp"
#include "timeline.hpp"

#include <algorithm>
#include <set>

namespace markettwin {
namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw RunError{"InvalidConfig", message};
  }
}

bool is_digest(std::string_view text) {
  return text.size() == 64 && std::ranges::all_of(text, [](char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

} // namespace

SimTime first_transition(const Shock& shock) {
  return std::visit(
      [](const auto& item) {
        if constexpr (requires { item.at; }) {
          return item.at;
        } else {
          return item.start;
        }
      },
      shock);
}

SimTime last_transition(const Shock& shock) {
  return std::visit(
      [](const auto& item) {
        if constexpr (requires { item.at; }) {
          return item.at;
        } else {
          return item.end;
        }
      },
      shock);
}

bool decides_at(const ModelDirectedParams& desk, SimTime anchor, SimTime at) noexcept {
  return at > anchor && (at.value() - anchor.value()) % desk.decision_interval.value() == 0;
}

SimTime stop_horizon(const RunConfig& config) noexcept {
  auto horizon = config.limits.horizon;
  for (const auto& stage : config.forks) {
    if (stage.horizon && *stage.horizon < horizon) {
      horizon = *stage.horizon;
    }
  }
  return horizon;
}

bool has_desk(const RunConfig& config) noexcept {
  const auto desk = [](const PolicyConfig& policy) {
    return std::holds_alternative<ModelDirectedParams>(policy);
  };
  if (config.scenario &&
      std::ranges::any_of(config.scenario->agents,
                          [&](const AgentConfig& agent) { return desk(agent.policy); })) {
    return true;
  }
  return std::ranges::any_of(config.forks, [&](const ForkStage& stage) {
    return std::ranges::any_of(stage.policy_overrides,
                               [&](const PolicyOverride& item) { return desk(item.policy); });
  });
}

namespace {

// The timeline as each stage leaves it: the shocks in force, which were removed, and each
// agent's policy and the time it took effect.
class StageChecker {
public:
  StageChecker(const RunConfig& config, const ScenarioConfig& base)
      : config_(&config), base_(&base), shocks_(base.shocks), removed_(shocks_.size(), false),
        anchors_(base.agents.size()), horizon_(config.limits.horizon) {
    policies_.reserve(base.agents.size());
    for (const auto& agent : base.agents) {
      policies_.push_back(agent.policy);
    }
  }

  void check(const ForkStage& stage) {
    require(!stage.parent_run_id.empty() && stage.parent_run_id.size() <= 128 &&
                is_digest(stage.parent_config_digest),
            "a fork stage needs a parent run ID and a SHA-256 parent config digest");
    require(stage.at >= previous_ && stage.at < horizon_,
            "fork stages must be in time order and before the horizon");
    if (stage.horizon) {
      require(*stage.horizon > stage.at && *stage.horizon <= horizon_,
              "a fork horizon may only shorten the run, to a time after the fork");
      horizon_ = *stage.horizon;
    }
    change_shocks(stage);
    change_policies(stage);
    check_future_shocks(stage.at);
    check_directives(stage);
    for (const auto& item : stage.recorded) {
      require(item.index >= 1 && !item.source.empty() && item.at.value() > 0,
              "recorded decisions need an index, a source, and a time");
    }
    previous_ = stage.at;
  }

private:
  [[nodiscard]] std::size_t slot_of(AgentId id) const {
    const auto& agents = base_->agents;
    const auto found = std::ranges::find(agents, id, &AgentConfig::id);
    require(found != agents.end(), "a fork stage names an agent that is not in the scenario");
    return static_cast<std::size_t>(std::distance(agents.begin(), found));
  }

  void change_shocks(const ForkStage& stage) {
    for (const auto index : stage.remove_shocks) {
      require(index < shocks_.size() && !removed_.at(index),
              "a fork may remove each existing shock once");
      require(first_transition(shocks_.at(index)) >= stage.at,
              "a fork may only remove a shock that has not started");
      removed_.at(index) = true;
    }
    for (const auto& shock : stage.add_shocks) {
      require(first_transition(shock) >= stage.at,
              "an added shock must start at or after the fork");
      shocks_.push_back(shock);
      removed_.push_back(false);
    }
    require(shocks_.size() <= 1'024, "too many shocks");
  }

  void change_policies(const ForkStage& stage) {
    std::set<AgentId> changed;
    for (const auto& item : stage.policy_overrides) {
      require(changed.insert(item.agent).second, "a fork stage overrides each agent once");
      const auto slot = slot_of(item.agent);
      auto agent = base_->agents.at(slot);
      agent.policy = item.policy;
      validate_agent(agent, config_->exchange);
      policies_.at(slot) = item.policy;
      anchors_.at(slot) = stage.at;
    }
  }

  // Shocks still to come are judged against the policies after the fork.
  void check_future_shocks(SimTime at) const {
    auto view = *base_;
    for (std::size_t slot = 0; slot < view.agents.size(); ++slot) {
      view.agents.at(slot).policy = policies_.at(slot);
    }
    view.shocks.clear();
    for (std::size_t index = 0; index < shocks_.size(); ++index) {
      if (!removed_.at(index) && last_transition(shocks_.at(index)) >= at) {
        view.shocks.push_back(shocks_.at(index));
      }
    }
    validate_shocks(view);
  }

  void check_directives(const ForkStage& stage) const {
    std::set<AgentId> directed;
    for (const auto& item : stage.directive_overrides) {
      require(directed.insert(item.agent).second, "a fork stage replaces each desk decision once");
      const auto slot = slot_of(item.agent);
      const auto* desk = std::get_if<ModelDirectedParams>(&policies_.at(slot));
      require(desk != nullptr && decides_at(*desk, anchors_.at(slot), stage.at),
              "a directive override needs an AI desk that decides at the fork time");
      const auto error = directive_error(item.directive, *desk, base_->agents.at(slot).limits);
      if (!error.empty()) {
        throw RunError{"InvalidConfig", "directive override: " + error};
      }
    }
  }

  const RunConfig* config_;
  const ScenarioConfig* base_;
  std::vector<Shock> shocks_;
  std::vector<bool> removed_;
  std::vector<PolicyConfig> policies_;
  std::vector<SimTime> anchors_;
  SimTime previous_;
  SimTime horizon_;
};

} // namespace

void validate_forks(const RunConfig& config) {
  if (config.forks.empty()) {
    return;
  }
  if (!config.scenario) {
    throw RunError{"InvalidConfig", "fork stages need a scenario"};
  }
  StageChecker checker{config, *config.scenario};
  for (const auto& stage : config.forks) {
    checker.check(stage);
  }
  auto parent = config;
  const auto& last = parent.forks.back();
  parent.run_id = last.parent_run_id;
  const auto expected = last.parent_config_digest;
  parent.forks.pop_back();
  if (sha256(canonical_config(parent)) != expected) {
    throw RunError{"ForkParentMismatch",
                   "removing the last fork stage does not give the parent's config"};
  }
}

DecisionResponse FixtureDecisionSource::decide(const DecisionRequest& request) {
  const auto observation = codec::parse(request.observation);
  const auto position = codec::signed_integer(observation.at("own").at("position"));
  const auto widest = codec::unsigned_integer(observation.at("limits").at("max_half_spread_ticks"));
  const auto cap = std::max<std::int64_t>(to_signed(request.current->max_position), 1);
  const auto held = position < 0 ? -position : position;
  auto directive = *request.current;
  if (held * 5 > cap * 4) {
    directive.mode = DeskMode::Reduce;
    directive.rationale = "inventory past four fifths of the cap; reduce toward flat";
  } else if (held * 2 > cap) {
    directive.mode = DeskMode::Quote;
    directive.half_spread_ticks = std::min(directive.half_spread_ticks + 1, widest);
    directive.skew_ticks = position > 0 ? -1 : 1;
    directive.rationale = "inventory past half the cap; widen and lean against it";
  } else {
    directive.mode = DeskMode::Quote;
    directive.half_spread_ticks = std::max<std::uint64_t>(directive.half_spread_ticks - 1, 2);
    directive.skew_ticks = 0;
    directive.rationale = "inventory within half the cap; quote both sides";
  }
  directive.half_spread_ticks = std::clamp<std::uint64_t>(directive.half_spread_ticks, 1, widest);
  return {
      .directive = directive,
      .source = "model",
      .model = "fixture",
      .input_tokens = 0,
      .output_tokens = 0,
      .error_code = {},
  };
}

} // namespace markettwin
