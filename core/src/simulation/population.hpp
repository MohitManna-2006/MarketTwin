#pragma once

#include "markettwin/simulation/fundamental.hpp"
#include "markettwin/simulation/simulation.hpp"

#include <memory>

namespace markettwin {

// The agents' side of a scenario run: each agent's delivered view of the world, its
// random streams, its limits, and the latency between it and the exchange. Policies see
// only what this class has delivered to them, and reach the exchange only through it.
class Population {
public:
  // The references must outlive the population. The fundamental is read only to derive
  // value agents' noisy estimates; no policy receives it.
  Population(const RunConfig& config, const ScenarioConfig& scenario,
             const FundamentalProcess& fundamental, Scheduler& scheduler);

  void start(); // schedules every agent's first wakeup, in slot order
  void wake(AgentSlot slot);
  void deliver(AgentSlot slot, const MarketDataUpdate& update);
  void deliver(AgentSlot slot, const OrderReport& report);
  void deliver(AgentSlot slot, const Fill& fill);

  // Exchange-side fan-out. Each schedules deliveries after the recipient's latency.
  void publish(const MarketDataUpdate& update);
  void route(const ExecutionReport& report);
  void route(const CancelResult& result);

  void set_quote_size(AgentId id, std::uint64_t quote_size_ppm);
  [[nodiscard]] std::vector<AgentActivity> activity() const;
  // Final state of every stream, four per agent in slot order.
  [[nodiscard]] std::vector<Rng::State> rng_states() const;

private:
  struct Agent {
    Agent(const AgentConfig& agent, std::uint64_t seed);

    const AgentConfig* config;
    std::unique_ptr<ITradingAgent> policy;
    Rng policy_rng;
    Rng wakeup_rng;
    Rng latency_rng;
    Rng signal_rng;
    std::optional<std::uint64_t> signal_noise_milliticks;
    std::vector<WorkingOrder> orders; // oldest first
    std::optional<MarketDataUpdate> market;
    AgentControls controls;
    std::int64_t position = 0;
    std::uint64_t orders_issued = 0;
    std::uint64_t messages = 0;
    SimTime order_channel; // latest delivery time already scheduled on each path
    SimTime report_channel;
    SimTime market_channel;
    AgentActivity activity;
  };

  [[nodiscard]] AgentObservation observe(const Agent& agent,
                                         std::optional<std::int64_t> estimate = {}) const;
  [[nodiscard]] std::optional<AgentSlot> slot_of(AgentId id) const noexcept;
  [[nodiscard]] SimTime order_arrival(Agent& agent);
  [[nodiscard]] SimTime report_delivery(Agent& agent) const;
  void flush(Agent& agent);
  [[nodiscard]] bool send(Agent& agent, const SubmitIntent& intent);
  [[nodiscard]] bool send(Agent& agent, const CancelIntent& intent);
  void record_fill(AgentId id, Side side, Quantity quantity);

  const RunConfig* config_;
  const ScenarioConfig* scenario_;
  const FundamentalProcess* fundamental_;
  Scheduler* scheduler_;
  std::vector<Agent> agents_;
  std::vector<std::pair<AgentId, AgentSlot>> slots_; // sorted by ID
  std::vector<Intent> intents_;                      // reused across callbacks
};

} // namespace markettwin
