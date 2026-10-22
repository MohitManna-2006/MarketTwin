#ifndef MARKETTWIN_SIMULATION_SCENARIO_HPP
#define MARKETTWIN_SIMULATION_SCENARIO_HPP

#include "markettwin/agents/desk.hpp"
#include "markettwin/agents/market_maker.hpp"
#include "markettwin/agents/momentum.hpp"
#include "markettwin/agents/noise.hpp"
#include "markettwin/agents/value.hpp"
#include "markettwin/exchange/matching_engine.hpp"
#include "markettwin/simulation/latency.hpp"

#include <memory>
#include <string_view>

namespace markettwin {

enum class WakeupDistribution { Fixed, Exponential };

struct WakeupConfig {
  SimTime first{0};
  SimTime interval{1'000'000'000}; // exact for Fixed, the mean for Exponential
  WakeupDistribution distribution = WakeupDistribution::Fixed;
  bool operator==(const WakeupConfig&) const = default;
};

using PolicyConfig = std::variant<MarketMakerParams, InventoryMarketMakerParams, MomentumParams,
                                  ValueParams, NoiseParams, ModelDirectedParams>;

// Replaces one agent's policy and nothing else. The agent keeps its ID, and with it its
// random streams, wakeup schedule, latency, and limits, so a variant or a fork differs
// from its base only in how that participant decides.
struct PolicyOverride {
  AgentId agent;
  PolicyConfig policy;
  bool operator==(const PolicyOverride&) const = default;
};

struct AgentConfig {
  AgentId id;
  WakeupConfig wakeup;
  LatencyConfig latency;
  AgentLimits limits;
  PolicyConfig policy;
  bool operator==(const AgentConfig&) const = default;
};

// Latent value in milliticks: a mean-reverting random walk stepped on a fixed interval.
//   value += (anchor - value) * mean_reversion_ppm / 1e6 + N(0, 1) * volatility
struct FundamentalConfig {
  std::int64_t initial_milliticks = 15'000'000;
  SimTime update_interval{1'000'000'000};
  std::uint64_t volatility_milliticks = 1'000; // standard deviation of one step
  std::uint64_t mean_reversion_ppm = 200;
  bool operator==(const FundamentalConfig&) const = default;
};

// Periodic depth snapshots written to the trace for later analysis.
struct SamplingConfig {
  SimTime interval{1'000'000'000};
  std::size_t depth = 5;
  bool operator==(const SamplingConfig&) const = default;
};

// Shifts the latent value and its anchor at one instant. Prices move only through the
// orders agents then choose to send.
struct FundamentalJump {
  SimTime at;
  std::int64_t delta_milliticks = 0;
  bool operator==(const FundamentalJump&) const = default;
};
// Scales the listed makers' quote sizes over [start, end). Zero withdraws them entirely.
struct LiquidityWithdrawal {
  SimTime start;
  SimTime end;
  std::vector<AgentId> agents;
  std::uint64_t quote_size_ppm = 0;
  bool operator==(const LiquidityWithdrawal&) const = default;
};
// Scales the fundamental's step volatility over [start, end).
struct VolatilityRegime {
  SimTime start;
  SimTime end;
  std::uint64_t volatility_ppm = kPpm;
  bool operator==(const VolatilityRegime&) const = default;
};
// Shifts the listed noise agents' order flow over [start, end): the probability that an
// order buys, the share of orders sent at market, and the arrival rate as ppm of each
// agent's configured wakeup rate. A changed rate applies from the agent's next scheduled
// wakeup, so the wakeup already queued when the shock begins or ends keeps its time.
struct OrderFlowImbalance {
  SimTime start;
  SimTime end;
  std::vector<AgentId> agents;
  std::uint64_t buy_probability_ppm = 500'000;
  std::uint64_t market_order_ppm = 300'000;
  std::uint64_t arrival_rate_ppm = kPpm;
  bool operator==(const OrderFlowImbalance&) const = default;
};
// Adds delay to each path of the listed agents over [start, end). Every path stays
// first-in first-out, so when the degradation ends a message never overtakes one sent
// while it was in force.
struct LatencyDegradation {
  SimTime start;
  SimTime end;
  std::vector<AgentId> agents;
  SimTime added_order_entry{0};
  SimTime added_report{0};
  SimTime added_market_data{0};
  bool operator==(const LatencyDegradation&) const = default;
};
// At one instant, each listed agent, in list order, asks to cancel every working order it
// has not already asked to cancel, oldest first. The cancels travel through the agent's own
// order-entry latency, jitter, and message budget, so they reach the exchange interleaved
// with everything else in flight. Policies are not changed: an agent requotes when its
// own logic next decides to.
struct QuoteWithdrawalStress {
  SimTime at;
  std::vector<AgentId> agents;
  bool operator==(const QuoteWithdrawalStress&) const = default;
};
using Shock = std::variant<FundamentalJump, LiquidityWithdrawal, VolatilityRegime,
                           OrderFlowImbalance, LatencyDegradation, QuoteWithdrawalStress>;

struct ScenarioConfig {
  PriceTicks reference_price{15'000};
  FundamentalConfig fundamental;
  std::optional<SamplingConfig> sampling;
  std::vector<AgentConfig> agents;
  std::vector<Shock> shocks;
  bool operator==(const ScenarioConfig&) const = default;
};

struct RunConfig;

// Agent-issued order IDs are (agent ID << 32) + n, so scripted IDs stay below 2^32.
inline constexpr std::uint64_t kAgentOrderIdShift = 32;

// Throws RunError{"InvalidConfig"} unless config.scenario is absent or fully in bounds.
void validate_scenario(const RunConfig& config);
// The per-agent and shock checks of validate_scenario, for configs a fork stage changes.
void validate_agent(const AgentConfig& agent, const ExchangeConfig& exchange);
void validate_shocks(const ScenarioConfig& scenario);
[[nodiscard]] std::string_view policy_name(const PolicyConfig& policy);
[[nodiscard]] std::unique_ptr<ITradingAgent> make_policy(const PolicyConfig& policy);

} // namespace markettwin
#endif // MARKETTWIN_SIMULATION_SCENARIO_HPP
