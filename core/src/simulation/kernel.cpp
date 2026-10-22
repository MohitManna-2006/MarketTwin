#include "markettwin/simulation/kernel.hpp"

#include "markettwin/common/hash.hpp"

#include "../accounting/accountant.hpp"
#include "../telemetry/recorder.hpp"
#include "population.hpp"
#include "timeline.hpp"

#include <memory>
#include <span>

namespace markettwin {
namespace {
using codec::Json;

// Live state that exists only when the config carries a scenario.
struct Scenario {
  Scenario(const RunConfig& run, const ScenarioConfig& scenario, Scheduler& scheduler)
      : config(&scenario), fundamental(scenario.fundamental, run.seed),
        population(run, scenario, fundamental, scheduler) {}

  const ScenarioConfig* config;
  FundamentalProcess fundamental;
  Population population;
  MarketDataUpdate feed; // last published top of book
  std::uint64_t shock_transitions = 0;
};

SchedulerLimits stop_limits(const RunConfig& config) {
  auto limits = config.limits;
  limits.horizon = stop_horizon(config);
  return limits;
}

// An AI desk's decision schedule and the directive it holds (ADR 0012).
struct Desk {
  AgentId id;
  ModelDirectedParams params;
  SimTime last; // the previous decision, or when the desk policy took effect
  SimTime next;
  std::uint64_t index = 0;
  Directive current;
};

Desk start_desk(AgentId id, const ModelDirectedParams& params, SimTime anchor) {
  return {
      .id = id,
      .params = params,
      .last = anchor,
      .next = SimTime{anchor.value() + params.decision_interval.value()},
      .index = 0,
      .current = params.initial,
  };
}

const RecordedDecision* find_decision(const ForkStage& stage, AgentId agent, SimTime at) {
  const auto found = std::ranges::find_if(stage.recorded, [&](const RecordedDecision& item) {
    return item.agent == agent && item.at == at;
  });
  return found == stage.recorded.end() ? nullptr : &*found;
}

class Run {
public:
  Run(const RunConfig& config, RecordSink& sink, MarketObserver* observer,
      DecisionSource* decisions)
      : config_(&config), observer_(observer), decisions_(decisions),
        scheduler_(stop_limits(config)), exchange_(config.exchange), recorder_(config, sink),
        accountant_(config, sink, recorder_.config_digest()) {
    if (config.scenario) {
      scenario_ = std::make_unique<Scenario>(config, *config.scenario, scheduler_);
      shocks_ = config.scenario->shocks;
      removed_.assign(shocks_.size(), false);
      for (const auto& agent : config.scenario->agents) {
        const auto* desk = std::get_if<ModelDirectedParams>(&agent.policy);
        desks_.push_back(desk != nullptr ? std::optional{start_desk(agent.id, *desk, SimTime{})}
                                         : std::nullopt);
      }
    }
  }

  RunSummary execute() {
    seed_queue();
    while (true) {
      // A stage can schedule shocks before the next queued event, so look again after each.
      if (const auto next = scheduler_.peek(); next && apply_due_barrier(*next)) {
        continue;
      }
      const auto event = scheduler_.next();
      if (!event) {
        break;
      }
      std::visit([&](const auto& payload) { handle(*event, payload); }, event->payload);
    }
    if (next_stage_ < config_->forks.size()) {
      throw RunError{"ForkNotReached", "the run stopped before a fork stage's time"};
    }
    return finish();
  }

private:
  // Scenario-only events cannot exist in a scripted run; reaching one is a kernel defect.
  [[nodiscard]] Scenario& scenario() {
    if (!scenario_) {
      throw RunError{"InternalError", "scenario event dispatched without a scenario"};
    }
    return *scenario_;
  }

  // Scripted commands first, so a version 1 config schedules exactly as it always did;
  // then shocks, the fundamental, sampling, and agents in slot order. Insertion order is
  // the tie-break at equal timestamps.
  void seed_queue() {
    for (const auto& item : config_->commands) {
      std::visit([&](const auto& command) { schedule(item.timestamp, command); }, item.command);
    }
    if (!scenario_) {
      return;
    }
    const auto& config = *scenario_->config;
    for (std::size_t index = 0; index < config.shocks.size(); ++index) {
      schedule_shock(index);
    }
    schedule(config.fundamental.update_interval, FundamentalTick{});
    if (config.sampling) {
      schedule(config.sampling->interval, MetricSample{.depth = config.sampling->depth});
    }
    scenario_->population.start();
  }

  void schedule_shock(std::size_t index) {
    std::visit(
        [&](const auto& shock) {
          if constexpr (requires { shock.at; }) {
            schedule(shock.at, ShockTransition{.shock = index, .phase = ShockPhase::Instant});
          } else {
            schedule_interval(index, shock.start, shock.end);
          }
        },
        shocks_.at(index));
  }

  // Applies the earliest fork stage or desk decision due at or before `next`, the time
  // of the next queued event. A stage goes before a decision at the same time, and
  // desks at one time decide in slot order. Returns whether anything was applied.
  bool apply_due_barrier(SimTime next) {
    const auto& forks = config_->forks;
    std::optional<AgentSlot> desk;
    for (AgentSlot slot = 0; slot < desks_.size(); ++slot) {
      if (desks_.at(slot) && desks_.at(slot)->next <= next &&
          (!desk || desks_.at(slot)->next < desks_.at(*desk)->next)) {
        desk = slot;
      }
    }
    if (next_stage_ < forks.size() && forks.at(next_stage_).at <= next &&
        (!desk || forks.at(next_stage_).at <= desks_.at(*desk)->next)) {
      apply_stage(next_stage_);
      ++next_stage_;
      return true;
    }
    if (desk) {
      decide(*desk);
      return true;
    }
    return false;
  }

  static Event barrier(SimTime at) {
    return {.timestamp = at, .scheduler_sequence = {}, .payload = StopCommand{}};
  }

  void apply_stage(std::size_t index) {
    const auto& stage = config_->forks.at(index);
    auto& live = scenario();
    auto record = codec::fork_stage(stage);
    record.erase("recorded_decisions");
    record.update({{"stage", index}, {"recorded_decision_count", stage.recorded.size()}});
    recorder_.outcome(barrier(stage.at), {}, "ForkPoint", record.dump());
    for (const auto shock : stage.remove_shocks) {
      removed_.at(shock) = true;
    }
    for (const auto& shock : stage.add_shocks) {
      shocks_.push_back(shock);
      removed_.push_back(false);
      schedule_shock(shocks_.size() - 1);
    }
    for (const auto& item : stage.policy_overrides) {
      live.population.replace_policy(item.agent, item.policy);
      const auto found = std::ranges::find(live.config->agents, item.agent, &AgentConfig::id);
      const auto slot = static_cast<std::size_t>(std::distance(live.config->agents.begin(), found));
      const auto* params = std::get_if<ModelDirectedParams>(&item.policy);
      desks_.at(slot) =
          params != nullptr ? std::optional{start_desk(item.agent, *params, stage.at)} : std::nullopt;
    }
  }

  // One desk decision (ADR 0012). A stage still ahead fixes it to the parent's record;
  // after the last stage its decision mode does; otherwise the decision source answers.
  void decide(AgentSlot slot) {
    auto& desk = desks_.at(slot).value();
    auto& live = scenario();
    const auto at = desk.next;
    ++desk.index;
    const auto observation =
        live.population.desk_observation(desk.id, at, desk.last, desk.params, desk.current);
    RecordedDecision decision{
        .agent = desk.id,
        .at = at,
        .index = desk.index,
        .source = {},
        .directive = desk.current,
        .model = {},
        .input_tokens = 0,
        .output_tokens = 0,
        .error_code = {},
        .observation_digest = sha256(observation),
    };
    const auto& forks = config_->forks;
    if (next_stage_ < forks.size()) {
      const auto* recorded = find_decision(forks.at(next_stage_), desk.id, at);
      if (recorded == nullptr || recorded->index != decision.index ||
          recorded->observation_digest != decision.observation_digest) {
        throw RunError{"DecisionMismatch", "a decision before the fork differs from the parent's"};
      }
      decision = *recorded;
    } else if (!forks.empty() && forks.back().decisions != DecisionMode::Live) {
      const auto& stage = forks.back();
      const auto override = std::ranges::find(stage.directive_overrides, desk.id,
                                              &DirectiveOverride::agent);
      if (stage.at == at && override != stage.directive_overrides.end()) {
        decision.source = "override";
        decision.directive = override->directive;
      } else if (const auto* recorded = find_decision(stage, desk.id, at);
                 stage.decisions == DecisionMode::Parent && recorded != nullptr) {
        decision.source = "parent";
        decision.directive = recorded->directive;
        decision.model = recorded->model;
      } else {
        decision.source = "hold";
      }
    } else if (!forks.empty() && forks.back().at == at &&
               std::ranges::find(forks.back().directive_overrides, desk.id,
                                 &DirectiveOverride::agent) !=
                   forks.back().directive_overrides.end()) {
      decision.source = "override";
      decision.directive = std::ranges::find(forks.back().directive_overrides, desk.id,
                                             &DirectiveOverride::agent)
                               ->directive;
    } else {
      ask(desk, observation, decision);
    }
    Json payload = codec::decision_fields(decision);
    payload.update({{"observation", codec::parse(observation)}});
    recorder_.outcome(barrier(at), {}, "Decision", payload.dump(), desk.id);
    desk.current = decision.directive;
    live.population.apply_directive(desk.id, desk.current);
    desk.last = at;
    desk.next = SimTime{at.value() + desk.params.decision_interval.value()};
  }

  void ask(const Desk& desk, std::string_view observation, RecordedDecision& decision) {
    if (decisions_ == nullptr) {
      throw RunError{"DecisionSourceMissing", "an AI desk decision needs a decision source"};
    }
    const auto response = decisions_->decide({
        .agent = desk.id,
        .at = decision.at,
        .index = decision.index,
        .mandate = desk.params.mandate,
        .observation = observation,
        .current = &desk.current,
    });
    decision.model = response.model;
    decision.input_tokens = response.input_tokens;
    decision.output_tokens = response.output_tokens;
    auto error = response.error_code;
    if (response.directive && error.empty()) {
      const auto reason =
          directive_error(*response.directive, desk.params,
                          scenario_->config->agents.at(slot_of(desk.id)).limits);
      if (reason.empty()) {
        decision.source = response.source;
        decision.directive = *response.directive;
        return;
      }
      error = "InvalidDirective";
    }
    // The previous directive stays in force.
    decision.source = "fallback";
    decision.error_code = error.empty() ? "SourceFailed" : error;
  }

  [[nodiscard]] std::size_t slot_of(AgentId id) const {
    const auto& agents = scenario_->config->agents;
    return static_cast<std::size_t>(
        std::distance(agents.begin(), std::ranges::find(agents, id, &AgentConfig::id)));
  }

  void schedule(SimTime timestamp, const EventPayload& payload) {
    static_cast<void>(scheduler_.schedule(timestamp, payload));
  }

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): begin precedes end, as in the config.
  void schedule_interval(std::size_t shock, SimTime begin, SimTime end) {
    schedule(begin, ShockTransition{.shock = shock, .phase = ShockPhase::Begin});
    schedule(end, ShockTransition{.shock = shock, .phase = ShockPhase::End});
  }

  [[nodiscard]] TopOfBook top() const noexcept {
    return {.bid = exchange_.book().top(Side::Buy), .ask = exchange_.book().top(Side::Sell)};
  }

  void handle(const Event& event, const Order& input) {
    const auto before = top();
    const auto sequence = recorder_.command(event, input);
    auto order = input;
    order.arrival_time = event.timestamp;
    order.arrival_sequence = sequence;
    const auto report = exchange_.submit(order);
    for (const auto& trade : report.trades) {
      recorder_.trade(event, sequence, trade);
    }
    summary_.trade_count += report.trades.size();
    for (const auto& status : report.order_updates) {
      recorder_.outcome(event, sequence, "OrderStatus", codec::status(status), status.owner,
                        status.order_id);
    }
    // The books are posted from the exchange's own report, at the moment of execution.
    // What each agent learns of it still travels through its report latency below.
    accountant_.on_order(event.timestamp, before, report);
    const auto after = top();
    accountant_.on_book(event.timestamp, after);
    if (observer_ != nullptr) {
      observer_->on_market(event.timestamp, report.trades, after);
    }
    if (scenario_) {
      scenario_->population.route(report);
      publish(event.timestamp, report.trades, after);
    }
  }

  void handle(const Event& event, const CancelCommand& cancel) {
    const auto sequence = recorder_.command(event, cancel);
    const auto result = exchange_.cancel(cancel.order_id, cancel.owner);
    recorder_.outcome(event, sequence, "CancelResult", codec::status(result.order), cancel.owner,
                      cancel.order_id);
    accountant_.on_cancel(result);
    const auto after = top();
    accountant_.on_book(event.timestamp, after);
    if (observer_ != nullptr) {
      observer_->on_market(event.timestamp, {}, after);
    }
    if (scenario_) {
      scenario_->population.route(result);
      publish(event.timestamp, {}, after);
    }
  }

  void handle(const Event& event, const SnapshotCommand& snapshot) {
    const auto sequence = recorder_.command(event, snapshot);
    const auto book = exchange_.snapshot(snapshot.depth);
    recorder_.outcome(event, sequence, "BookSnapshot", codec::snapshot(book).dump());
    // Every depth snapshot, scripted or periodic, is also one point of the time series.
    accountant_.sample(event.timestamp, book, snapshot.depth,
                       scenario_ ? std::optional{scenario_->fundamental.value()} : std::nullopt);
  }

  void handle(const Event& event, const StopCommand& stop) {
    static_cast<void>(recorder_.command(event, stop));
    scheduler_.request_stop();
  }

  void handle(const Event& /*event*/, const AgentWakeup& wakeup) {
    scenario().population.wake(wakeup.slot);
  }
  void handle(const Event& /*event*/, const MarketDataDelivery& delivery) {
    scenario().population.deliver(delivery.slot, delivery.update);
  }
  void handle(const Event& /*event*/, const OrderReportDelivery& delivery) {
    scenario().population.deliver(delivery.slot, delivery.report);
  }
  void handle(const Event& /*event*/, const FillDelivery& delivery) {
    scenario().population.deliver(delivery.slot, delivery.fill);
  }

  void handle(const Event& event, const FundamentalTick& tick) {
    auto& live = scenario();
    live.fundamental.step();
    recorder_.outcome(event, {}, "FundamentalValueUpdate",
                      codec::fundamental_update(live.fundamental).dump());
    static_cast<void>(scheduler_.schedule_after(live.config->fundamental.update_interval, tick));
  }

  void handle(const Event& event, const ShockTransition& transition) {
    auto& live = scenario();
    if (removed_.at(transition.shock)) {
      return; // removed by a fork stage before it started
    }
    const auto& shock = shocks_.at(transition.shock);
    const bool begin = transition.phase == ShockPhase::Begin;
    struct Apply {
      Scenario* live;
      bool begin;
      void operator()(const FundamentalJump& jump) const {
        live->fundamental.jump(jump.delta_milliticks);
      }
      void operator()(const LiquidityWithdrawal& withdrawal) const {
        for (const auto agent : withdrawal.agents) {
          live->population.set_quote_size(agent, begin ? withdrawal.quote_size_ppm : kPpm);
        }
      }
      void operator()(const VolatilityRegime& regime) const {
        live->fundamental.set_volatility_ppm(begin ? regime.volatility_ppm : kPpm);
      }
      void operator()(const OrderFlowImbalance& imbalance) const {
        for (const auto agent : imbalance.agents) {
          live->population.set_order_flow(agent, begin ? &imbalance : nullptr);
        }
      }
      void operator()(const LatencyDegradation& degradation) const {
        for (const auto agent : degradation.agents) {
          live->population.set_added_latency(agent, begin ? &degradation : nullptr);
        }
      }
      void operator()(const QuoteWithdrawalStress& stress) const {
        for (const auto agent : stress.agents) {
          live->population.withdraw_quotes(agent);
        }
      }
    };
    std::visit(Apply{.live = &live, .begin = begin}, shock);
    ++live.shock_transitions;
    recorder_.outcome(event, {}, "ScenarioShock",
                      codec::shock_transition(transition.shock, transition.phase, shock).dump());
  }

  void handle(const Event& event, const MetricSample& sample) {
    handle(event, SnapshotCommand{.depth = sample.depth});
    if (const auto& sampling = scenario().config->sampling) {
      static_cast<void>(scheduler_.schedule_after(sampling->interval, sample));
    }
  }

  // One update per exchange command that traded or changed the best bid or ask.
  void publish(SimTime now, std::span<const Trade> trades, const TopOfBook& top) {
    auto& feed = scenario_->feed;
    if (trades.empty() && top.bid == feed.best_bid && top.ask == feed.best_ask) {
      return;
    }
    feed.sequence = Sequence{feed.sequence.value() + 1};
    feed.exchange_time = now;
    feed.best_bid = top.bid;
    feed.best_ask = top.ask;
    for (const auto& trade : trades) {
      feed.last_trade_price = trade.price_ticks;
      feed.cumulative_volume = checked_add(feed.cumulative_volume, trade.quantity);
    }
    scenario_->population.publish(feed);
  }

  RunSummary finish() {
    exchange_.verify_invariants();
    const auto stop_reason = scheduler_.stop_reason();
    if (!stop_reason) {
      throw RunError{"InternalError", "scheduler drained without a terminal reason"};
    }
    summary_.seed = config_->seed;
    summary_.event_count = scheduler_.dispatched();
    summary_.stop_reason = *stop_reason;
    summary_.final_time = scheduler_.now();
    summary_.resting_order_count = exchange_.book().size();
    summary_.book_digest = codec::book_digest(exchange_);
    if (scenario_) {
      const auto& fundamental = scenario_->fundamental;
      summary_.scenario = ScenarioSummary{
          .fundamental_milliticks = fundamental.value(),
          .fundamental_updates = fundamental.updates(),
          .market_data_updates = scenario_->feed.sequence.value(),
          .shock_transitions = scenario_->shock_transitions,
          .rng_digest =
              codec::rng_digest(fundamental.rng().state(), scenario_->population.rng_states()),
          .agents = scenario_->population.activity(),
      };
    }
    recorder_.finish(summary_);
    accountant_.finish(summary_, exchange_,
                       scenario_ ? scenario_->population.cash_refusals()
                                 : std::vector<std::pair<AgentId, std::uint64_t>>{});
    return summary_;
  }

  const RunConfig* config_;
  MarketObserver* observer_;
  DecisionSource* decisions_;
  Scheduler scheduler_;
  MatchingEngine exchange_;
  Recorder recorder_;
  Accountant accountant_;
  std::unique_ptr<Scenario> scenario_;
  std::vector<Shock> shocks_; // the scenario's, then each fork stage's additions
  std::vector<bool> removed_;
  std::size_t next_stage_ = 0;
  std::vector<std::optional<Desk>> desks_; // by slot
  RunSummary summary_;
};

} // namespace

RunSummary run_kernel(const RunConfig& config, RecordSink& sink, MarketObserver* observer,
                      DecisionSource* decisions) {
  validate_config(config);
  return Run{config, sink, observer, decisions}.execute();
}

} // namespace markettwin
