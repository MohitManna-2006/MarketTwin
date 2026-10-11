#ifndef MARKETTWIN_AGENTS_AGENT_HPP
#define MARKETTWIN_AGENTS_AGENT_HPP

#include "markettwin/agents/messages.hpp"
#include "markettwin/common/math.hpp"
#include "markettwin/common/rng.hpp"

#include <span>
#include <variant>
#include <vector>

namespace markettwin {

// Upper bound on any price an agent may send or a scenario may script. Keeps every
// policy's sub-tick arithmetic inside 64 bits without per-operation checks.
inline constexpr std::int64_t kMaxScenarioPriceTicks = 1'000'000'000'000;

// An order the agent has sent and not yet seen finished. Built only from the agent's own
// intents and delivered reports, so it lags the exchange by the report latency.
struct WorkingOrder {
  OrderId id;
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  PriceTicks price;
  Quantity remaining;
  TimeInForce tif = TimeInForce::Gtc;
  SimTime sent_at;
  bool acknowledged = false;
  bool cancel_requested = false;
  bool operator==(const WorkingOrder&) const = default;
};

struct SubmitIntent {
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  PriceTicks limit_price;
  Quantity quantity;
  TimeInForce tif = TimeInForce::Gtc;
  bool operator==(const SubmitIntent&) const = default;
};
struct CancelIntent {
  OrderId order_id;
  bool operator==(const CancelIntent&) const = default;
};
using Intent = std::variant<SubmitIntent, CancelIntent>;

// Hard per-agent limits enforced by the environment before an intent becomes a message.
struct AgentLimits {
  std::uint64_t max_position = 1'000;
  Quantity max_order_quantity{100};
  std::size_t max_open_orders = 16;
  std::uint64_t max_messages = 1'000'000;
  bool operator==(const AgentLimits&) const = default;
};

// Scenario-imposed adjustments. Shocks change these; policies must honor them.
struct AgentControls {
  std::uint64_t quote_size_ppm = kPpm;
  bool operator==(const AgentControls&) const = default;
};

// Everything a policy may know. There is deliberately no access to the book, the
// scheduler, other agents, or the latent fundamental value.
struct AgentObservation {
  SimTime now;
  SimTime horizon;
  PriceTicks reference_price; // public scenario reference, used before any market data
  std::optional<MarketDataUpdate> market;
  std::span<const WorkingOrder> open_orders;
  std::int64_t position = 0; // signed shares from delivered fills
  AgentLimits limits;
  AgentControls controls;
  std::optional<std::int64_t> fundamental_estimate_milliticks;
};

// Collects a policy's intents and exposes its private random stream.
class AgentActions {
public:
  AgentActions(Rng& rng, std::vector<Intent>& intents) noexcept : rng_(&rng), intents_(&intents) {}
  [[nodiscard]] Rng& rng() noexcept { return *rng_; }
  void submit(const SubmitIntent& intent) { intents_->emplace_back(intent); }
  void cancel(OrderId order_id) { intents_->emplace_back(CancelIntent{order_id}); }

private:
  Rng* rng_;
  std::vector<Intent>* intents_;
};

// A policy turns observations into intents. It never touches the book: the environment
// applies limits and latency, and the exchange decides the outcome. Handlers must be
// deterministic functions of the policy's state, the observation, and actions.rng().
class ITradingAgent {
public:
  ITradingAgent() = default;
  virtual ~ITradingAgent() = default;
  ITradingAgent(const ITradingAgent&) = delete;
  ITradingAgent& operator=(const ITradingAgent&) = delete;
  ITradingAgent(ITradingAgent&&) = delete;
  ITradingAgent& operator=(ITradingAgent&&) = delete;

  virtual void on_market_data(const AgentObservation& observation, AgentActions& actions) = 0;
  virtual void on_timer(const AgentObservation& observation, AgentActions& actions) = 0;
  virtual void on_order_report(const OrderReport& report) = 0;
  virtual void on_execution(const Fill& fill) = 0;
};

// Whether the delivered market data carries a price (a two-sided book or a print).
[[nodiscard]] bool has_market_price(const AgentObservation& observation) noexcept;
// Twice the reference price in ticks, so a half-tick midpoint stays an integer:
// midpoint when both sides are quoted, else the last trade, else the scenario reference.
[[nodiscard]] std::int64_t reference_twice(const AgentObservation& observation) noexcept;
// Shares the agent may still add on a side, counting every working order on that side.
[[nodiscard]] std::uint64_t side_capacity(const AgentObservation& observation, Side side) noexcept;

} // namespace markettwin
#endif // MARKETTWIN_AGENTS_AGENT_HPP
