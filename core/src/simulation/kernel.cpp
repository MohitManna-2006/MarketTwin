#include "markettwin/simulation/kernel.hpp"

#include "../accounting/accountant.hpp"
#include "../telemetry/recorder.hpp"
#include "population.hpp"

#include <memory>
#include <span>

namespace markettwin {
namespace {

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

class Run {
public:
  Run(const RunConfig& config, RecordSink& sink, MarketObserver* observer)
      : config_(&config), observer_(observer), scheduler_(config.limits),
        exchange_(config.exchange), recorder_(config, sink),
        accountant_(config, sink, recorder_.config_digest()) {
    if (config.scenario) {
      scenario_ = std::make_unique<Scenario>(config, *config.scenario, scheduler_);
    }
  }

  RunSummary execute() {
    seed_queue();
    while (const auto event = scheduler_.next()) {
      std::visit([&](const auto& payload) { handle(*event, payload); }, event->payload);
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
      std::visit(
          [&](const auto& shock) {
            if constexpr (requires { shock.at; }) {
              schedule(shock.at, ShockTransition{.shock = index, .phase = ShockPhase::Instant});
            } else {
              schedule_interval(index, shock.start, shock.end);
            }
          },
          config.shocks.at(index));
    }
    schedule(config.fundamental.update_interval, FundamentalTick{});
    if (config.sampling) {
      schedule(config.sampling->interval, MetricSample{.depth = config.sampling->depth});
    }
    scenario_->population.start();
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
    const auto& shock = live.config->shocks.at(transition.shock);
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
  Scheduler scheduler_;
  MatchingEngine exchange_;
  Recorder recorder_;
  Accountant accountant_;
  std::unique_ptr<Scenario> scenario_;
  RunSummary summary_;
};

} // namespace

RunSummary run_kernel(const RunConfig& config, RecordSink& sink, MarketObserver* observer) {
  validate_config(config);
  return Run{config, sink, observer}.execute();
}

} // namespace markettwin
