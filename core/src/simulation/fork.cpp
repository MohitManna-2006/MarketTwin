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

void validate_forks(const RunConfig& config) {
  if (config.forks.empty()) {
    return;
  }
  require(config.scenario.has_value(), "fork stages need a scenario");
  const auto& base = *config.scenario;
  std::vector<Shock> shocks = base.shocks;
  std::vector<bool> removed(shocks.size(), false);
  std::vector<PolicyConfig> policies;
  std::vector<SimTime> anchors(base.agents.size());
  for (const auto& agent : base.agents) {
    policies.push_back(agent.policy);
  }
  const auto slot_of = [&](AgentId id) -> std::size_t {
    const auto found = std::ranges::find(base.agents, id, &AgentConfig::id);
    require(found != base.agents.end(), "a fork stage names an agent that is not in the scenario");
    return static_cast<std::size_t>(std::distance(base.agents.begin(), found));
  };
  SimTime previous;
  auto horizon = config.limits.horizon;
  for (const auto& stage : config.forks) {
    require(!stage.parent_run_id.empty() && stage.parent_run_id.size() <= 128 &&
                is_digest(stage.parent_config_digest),
            "a fork stage needs a parent run ID and a SHA-256 parent config digest");
    require(stage.at >= previous && stage.at < horizon,
            "fork stages must be in time order and before the horizon");
    if (stage.horizon) {
      require(*stage.horizon > stage.at && *stage.horizon <= horizon,
              "a fork horizon may only shorten the run, to a time after the fork");
      horizon = *stage.horizon;
    }
    for (const auto index : stage.remove_shocks) {
      require(index < shocks.size() && !removed.at(index),
              "a fork may remove each existing shock once");
      require(first_transition(shocks.at(index)) >= stage.at,
              "a fork may only remove a shock that has not started");
      removed.at(index) = true;
    }
    for (const auto& shock : stage.add_shocks) {
      require(first_transition(shock) >= stage.at, "an added shock must start at or after the fork");
      shocks.push_back(shock);
      removed.push_back(false);
    }
    require(shocks.size() <= 1'024, "too many shocks");
    std::set<AgentId> changed;
    for (const auto& item : stage.policy_overrides) {
      require(changed.insert(item.agent).second, "a fork stage overrides each agent once");
      const auto slot = slot_of(item.agent);
      auto agent = base.agents.at(slot);
      agent.policy = item.policy;
      validate_agent(agent, config.exchange);
      policies.at(slot) = item.policy;
      anchors.at(slot) = stage.at;
    }
    // Shocks still to come are judged against the policies after the fork.
    auto view = base;
    for (std::size_t slot = 0; slot < view.agents.size(); ++slot) {
      view.agents.at(slot).policy = policies.at(slot);
    }
    view.shocks.clear();
    for (std::size_t index = 0; index < shocks.size(); ++index) {
      if (!removed.at(index) && last_transition(shocks.at(index)) >= stage.at) {
        view.shocks.push_back(shocks.at(index));
      }
    }
    validate_shocks(view);
    std::set<AgentId> directed;
    for (const auto& item : stage.directive_overrides) {
      require(directed.insert(item.agent).second, "a fork stage replaces each desk decision once");
      const auto slot = slot_of(item.agent);
      const auto* desk = std::get_if<ModelDirectedParams>(&policies.at(slot));
      require(desk != nullptr && decides_at(*desk, anchors.at(slot), stage.at),
              "a directive override needs an AI desk that decides at the fork time");
      const auto error = directive_error(item.directive, *desk, base.agents.at(slot).limits);
      if (!error.empty()) {
        throw RunError{"InvalidConfig", "directive override: " + error};
      }
    }
    for (const auto& item : stage.recorded) {
      require(item.index >= 1 && !item.source.empty() && item.at.value() > 0,
              "recorded decisions need an index, a source, and a time");
    }
    previous = stage.at;
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
  const auto widest =
      codec::unsigned_integer(observation.at("limits").at("max_half_spread_ticks"));
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
