#include "markettwin/simulation/config.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <utility>

namespace markettwin {
namespace {

constexpr std::uint64_t kMaxDuration = 1'000'000'000'000'000; // about 11.5 days of ns
constexpr std::uint64_t kMaxHorizon = 1'000'000'000'000'000'000;
constexpr std::uint64_t kMaxShares = 1'000'000;
constexpr std::uint64_t kMaxTicks = 1'000'000'000;
constexpr std::uint64_t kMaxMilliticks = 1'000'000'000'000;
constexpr std::size_t kMaxAgents = 1'024;
constexpr std::size_t kMaxShocks = 1'024;
constexpr std::size_t kMaxWindow = 4'096;
constexpr std::size_t kMaxQuoteLevels = 32;
constexpr std::uint64_t kMaxVolatilityPpm = 100 * kPpm;
constexpr std::uint64_t kMinArrivalRatePpm = kPpm / 100;
constexpr std::uint64_t kMaxArrivalRatePpm = 100 * kPpm;

void require(bool condition, const char* message) {
  if (!condition) {
    throw RunError{"InvalidConfig", message};
  }
}

void check(const MarketMakerParams& params, const AgentConfig& agent) {
  require(params.half_spread_ticks >= 1 && params.half_spread_ticks <= kMaxTicks,
          "maker half spread must be 1 to 1e9 ticks");
  require(params.quote_size.value() >= 1 && params.quote_size <= agent.limits.max_order_quantity,
          "maker quote size must be positive and within the agent's order limit");
  require(params.levels >= 1 && params.levels <= kMaxQuoteLevels &&
              2 * params.levels <= agent.limits.max_open_orders,
          "maker levels must fit twice within max_open_orders");
  require(params.level_spacing_ticks >= 1 && params.level_spacing_ticks <= kMaxTicks,
          "maker level spacing must be 1 to 1e9 ticks");
}

void check(const InventoryMarketMakerParams& params, const AgentConfig& agent) {
  check(params.quoting, agent);
  require(params.risk_aversion_ppm <= kPpm && params.volatility_alpha_ppm <= kPpm,
          "inventory maker ppm parameters must not exceed 1e6");
  require(params.volatility_floor_milliticks <= 1'000'000 && params.max_skew_ticks <= kMaxTicks,
          "inventory maker volatility floor or skew cap is out of range");
}

void check(const MomentumParams& params, const AgentConfig& agent) {
  require(params.short_window >= 1 && params.short_window < params.long_window &&
              params.long_window <= kMaxWindow,
          "momentum windows must satisfy 1 <= short < long <= 4096");
  require(params.threshold_milliticks <= kMaxMilliticks && params.slippage_ticks <= kMaxTicks &&
              params.cooldown.value() <= kMaxDuration,
          "momentum threshold, slippage, or cooldown is out of range");
  require(params.order_size.value() >= 1 && params.order_size <= agent.limits.max_order_quantity,
          "momentum order size must be positive and within the agent's order limit");
}

void check(const ValueParams& params, const AgentConfig& agent) {
  require(params.estimate_noise_milliticks <= kMaxMilliticks / 1'000 &&
              params.threshold_milliticks <= kMaxMilliticks && params.slippage_ticks <= kMaxTicks,
          "value agent noise, threshold, or slippage is out of range");
  require(params.order_size.value() >= 1 && params.order_size <= agent.limits.max_order_quantity,
          "value order size must be positive and within the agent's order limit");
}

void check(const NoiseParams& params, const AgentConfig& agent) {
  require(params.buy_probability_ppm <= kPpm && params.market_order_ppm <= kPpm,
          "noise probabilities must not exceed 1e6 ppm");
  require(params.min_quantity.value() >= 1 && params.min_quantity <= params.max_quantity &&
              params.max_quantity <= agent.limits.max_order_quantity,
          "noise quantities must satisfy 1 <= min <= max <= the agent's order limit");
  require(params.limit_offset_ticks <= kMaxTicks, "noise limit offset exceeds 1e9 ticks");
  require(params.max_resting_orders >= 1 &&
              params.max_resting_orders < agent.limits.max_open_orders,
          "noise max_resting_orders must be positive and below max_open_orders");
}

void check(const ModelDirectedParams& params, const AgentConfig& agent) {
  require(params.decision_interval.value() >= 1'000'000 &&
              params.decision_interval.value() <= kMaxDuration,
          "desk decision interval must be 1 ms to 1e15 ns");
  require(params.max_half_spread_ticks >= 1 && params.max_half_spread_ticks <= kMaxTicks &&
              params.max_skew_ticks <= kMaxTicks,
          "desk spread and skew bounds are out of range");
  require(params.mandate.size() <= kMaxMandateBytes &&
              std::ranges::all_of(params.mandate,
                                  [](char ch) { return ch == '\n' || (ch >= ' ' && ch <= '~'); }),
          "desk mandate must be at most 2000 printable ASCII characters");
  const auto error = directive_error(params.initial, params, agent.limits);
  if (!error.empty()) {
    throw RunError{"InvalidConfig", "desk initial directive: " + error};
  }
}

void check_agent(const AgentConfig& agent, const ExchangeConfig& exchange) {
  require(agent.id.value() >= 1 && agent.id.value() < (1ULL << 31U),
          "scenario agent IDs must be 1 to 2^31 - 1");
  require(std::ranges::find(exchange.agents, agent.id) != exchange.agents.end(),
          "scenario agent is not registered with the exchange");
  require(agent.wakeup.interval.value() >= 1 && agent.wakeup.interval.value() <= kMaxDuration &&
              agent.wakeup.first.value() <= kMaxHorizon,
          "agent wakeup interval must be 1 ns to 1e15 ns");
  require(agent.wakeup.distribution == WakeupDistribution::Fixed ||
              agent.wakeup.distribution == WakeupDistribution::Exponential,
          "unknown wakeup distribution");
  require(agent.latency.order_entry.value() <= kMaxDuration &&
              agent.latency.order_entry_jitter.value() <= kMaxDuration &&
              agent.latency.report.value() <= kMaxDuration &&
              agent.latency.market_data.value() <= kMaxDuration,
          "agent latency exceeds 1e15 ns");
  require(agent.limits.max_position >= 1 && agent.limits.max_position <= kMaxShares,
          "agent max_position must be 1 to 1e6 shares");
  require(agent.limits.max_order_quantity.value() >= 1 &&
              agent.limits.max_order_quantity.value() <= kMaxShares &&
              agent.limits.max_order_quantity <= exchange.max_order_quantity,
          "agent max_order_quantity must be positive and within exchange and 1e6 limits");
  require(agent.limits.max_open_orders >= 1 && agent.limits.max_open_orders <= 4'096,
          "agent max_open_orders must be 1 to 4096");
  require(agent.limits.max_messages >= 1 && agent.limits.max_messages < (1ULL << 32U),
          "agent max_messages must be 1 to 2^32 - 1");
  std::visit([&](const auto& params) { check(params, agent); }, agent.policy);
}

struct Interval {
  SimTime start;
  SimTime end;
};

void check_interval(SimTime start, SimTime end) {
  require(start < end && end.value() <= kMaxHorizon, "shock interval must satisfy start < end");
}

void require_disjoint(std::vector<Interval>& intervals, const char* message) {
  std::ranges::sort(intervals, {}, &Interval::start);
  for (std::size_t index = 1; index < intervals.size(); ++index) {
    require(intervals.at(index - 1).end <= intervals.at(index).start, message);
  }
}

using AgentIntervals = std::vector<std::pair<AgentId, Interval>>;

// A desk quotes through the same reconciliation and honors the same quote-size control.
bool is_maker(const PolicyConfig& policy) {
  return std::holds_alternative<MarketMakerParams>(policy) ||
         std::holds_alternative<InventoryMarketMakerParams>(policy) ||
         std::holds_alternative<ModelDirectedParams>(policy);
}

// Every named agent exists and passes `allowed`; with an interval, each is recorded for
// the per-agent overlap check of its shock kind.
template <typename Allowed>
void check_agents(const ScenarioConfig& scenario, const std::vector<AgentId>& agents,
                  Allowed allowed, const char* message, AgentIntervals* intervals = nullptr,
                  Interval interval = {}) {
  require(!agents.empty(), message);
  std::set<AgentId> seen;
  for (const auto id : agents) {
    const auto found = std::ranges::find(scenario.agents, id, &AgentConfig::id);
    require(found != scenario.agents.end() && allowed(found->policy) && seen.insert(id).second,
            message);
    if (intervals != nullptr) {
      intervals->emplace_back(id, interval);
    }
  }
}

void require_disjoint_per_agent(AgentIntervals& items, const char* message) {
  std::ranges::sort(items, {}, [](const auto& item) { return item.first; });
  for (auto first = items.begin(); first != items.end();) {
    const auto last = std::find_if(first, items.end(),
                                   [&](const auto& item) { return item.first != first->first; });
    std::vector<Interval> intervals;
    for (auto item = first; item != last; ++item) {
      intervals.push_back(item->second);
    }
    require_disjoint(intervals, message);
    first = last;
  }
}

// Overlap would need a combination rule; each check requires the config to be
// unambiguous instead.
struct ShockChecker {
  explicit ShockChecker(const ScenarioConfig& config) : scenario(&config) {}

  const ScenarioConfig* scenario;
  std::vector<Interval> regimes;
  AgentIntervals withdrawals;
  AgentIntervals imbalances;
  AgentIntervals degradations;

  void operator()(const FundamentalJump& jump) const {
    require(jump.at.value() <= kMaxHorizon && jump.delta_milliticks != 0 &&
                jump.delta_milliticks >= -to_signed(kMaxMilliticks) &&
                jump.delta_milliticks <= to_signed(kMaxMilliticks),
            "fundamental jump must be nonzero and within 1e12 milliticks");
  }
  void operator()(const LiquidityWithdrawal& withdrawal) {
    check_interval(withdrawal.start, withdrawal.end);
    require(withdrawal.quote_size_ppm <= kPpm,
            "liquidity withdrawal quote size must be at most 1e6 ppm");
    check_agents(*scenario, withdrawal.agents, is_maker,
                 "liquidity withdrawal must name distinct market-maker agents", &withdrawals,
                 {.start = withdrawal.start, .end = withdrawal.end});
  }
  void operator()(const VolatilityRegime& regime) {
    check_interval(regime.start, regime.end);
    require(regime.volatility_ppm <= kMaxVolatilityPpm, "volatility regime exceeds 100x");
    regimes.push_back({.start = regime.start, .end = regime.end});
  }
  void operator()(const OrderFlowImbalance& imbalance) {
    check_interval(imbalance.start, imbalance.end);
    require(imbalance.buy_probability_ppm <= kPpm && imbalance.market_order_ppm <= kPpm,
            "order-flow imbalance probabilities must not exceed 1e6 ppm");
    require(imbalance.arrival_rate_ppm >= kMinArrivalRatePpm &&
                imbalance.arrival_rate_ppm <= kMaxArrivalRatePpm,
            "order-flow imbalance arrival rate must be 0.01x to 100x");
    check_agents(
        *scenario, imbalance.agents,
        [](const PolicyConfig& policy) { return std::holds_alternative<NoiseParams>(policy); },
        "order-flow imbalance must name distinct noise agents", &imbalances,
        {.start = imbalance.start, .end = imbalance.end});
  }
  void operator()(const LatencyDegradation& degradation) {
    check_interval(degradation.start, degradation.end);
    const auto order_entry = degradation.added_order_entry.value();
    const auto report = degradation.added_report.value();
    const auto market_data = degradation.added_market_data.value();
    require(order_entry <= kMaxDuration && report <= kMaxDuration && market_data <= kMaxDuration &&
                (order_entry != 0 || report != 0 || market_data != 0),
            "latency degradation must add 1 ns to 1e15 ns on at least one path");
    check_agents(*scenario, degradation.agents, [](const PolicyConfig& /*policy*/) { return true; },
                 "latency degradation must name distinct scenario agents", &degradations,
                 {.start = degradation.start, .end = degradation.end});
  }
  void operator()(const QuoteWithdrawalStress& stress) const {
    require(stress.at.value() <= kMaxHorizon, "quote withdrawal stress time exceeds 1e18 ns");
    check_agents(
        *scenario, stress.agents, [](const PolicyConfig& /*policy*/) { return true; },
        "quote withdrawal stress must name distinct scenario agents");
  }
};

void check_shocks(const ScenarioConfig& scenario) {
  require(scenario.shocks.size() <= kMaxShocks, "too many shocks");
  ShockChecker checker{scenario};
  for (const auto& shock : scenario.shocks) {
    std::visit(checker, shock);
  }
  require_disjoint(checker.regimes, "volatility regimes must not overlap");
  require_disjoint_per_agent(checker.withdrawals,
                             "liquidity withdrawals must not overlap for one agent");
  require_disjoint_per_agent(checker.imbalances,
                             "order-flow imbalances must not overlap for one agent");
  require_disjoint_per_agent(checker.degradations,
                             "latency degradations must not overlap for one agent");
}

} // namespace

void validate_agent(const AgentConfig& agent, const ExchangeConfig& exchange) {
  check_agent(agent, exchange);
}

void validate_shocks(const ScenarioConfig& scenario) {
  check_shocks(scenario);
}

void validate_scenario(const RunConfig& config) {
  if (!config.scenario) {
    return;
  }
  const auto& scenario = *config.scenario;
  require(config.limits.horizon.value() <= kMaxHorizon, "scenario horizon exceeds 1e18 ns");
  require(scenario.reference_price.value() >= 1 &&
              scenario.reference_price.value() <= to_signed(kMaxTicks),
          "reference price must be 1 to 1e9 ticks");
  const auto& fundamental = scenario.fundamental;
  require(fundamental.initial_milliticks >= kMilliticks &&
              fundamental.initial_milliticks <= to_signed(kMaxMilliticks),
          "fundamental initial value must be one tick to 1e12 milliticks");
  require(fundamental.update_interval.value() >= 1 &&
              fundamental.update_interval.value() <= kMaxDuration,
          "fundamental update interval must be 1 ns to 1e15 ns");
  require(fundamental.volatility_milliticks <= kMaxTicks && fundamental.mean_reversion_ppm <= kPpm,
          "fundamental volatility or mean reversion is out of range");
  if (scenario.sampling) {
    require(scenario.sampling->interval.value() >= 1 &&
                scenario.sampling->interval.value() <= kMaxDuration &&
                scenario.sampling->depth >= 1 &&
                scenario.sampling->depth <= config.exchange.max_snapshot_depth,
            "sampling interval or depth is out of range");
  }
  require(scenario.agents.size() <= kMaxAgents, "too many scenario agents");
  std::set<AgentId> seen;
  for (const auto& agent : scenario.agents) {
    require(seen.insert(agent.id).second, "scenario agent IDs must be unique");
    check_agent(agent, config.exchange);
  }
  check_shocks(scenario);
  for (const auto& item : config.commands) {
    if (const auto* order = std::get_if<Order>(&item.command)) {
      require(order->id.value() < (1ULL << kAgentOrderIdShift) &&
                  order->limit_price.value() <= kMaxScenarioPriceTicks,
              "scripted orders in a scenario need an ID below 2^32 and a price within 1e12 ticks");
    }
  }
}

std::string_view policy_name(const PolicyConfig& policy) {
  constexpr std::array<std::string_view, 6> kNames{
      "MarketMaker", "InventoryMarketMaker", "Momentum", "Value", "Noise", "ModelDirected",
  };
  return kNames.at(policy.index()); // throws for a valueless variant
}

std::unique_ptr<ITradingAgent> make_policy(const PolicyConfig& policy) {
  struct Factory {
    std::unique_ptr<ITradingAgent> operator()(const MarketMakerParams& params) const {
      return std::make_unique<MarketMaker>(params);
    }
    std::unique_ptr<ITradingAgent> operator()(const InventoryMarketMakerParams& params) const {
      return std::make_unique<InventoryMarketMaker>(params);
    }
    std::unique_ptr<ITradingAgent> operator()(const MomentumParams& params) const {
      return std::make_unique<MomentumAgent>(params);
    }
    std::unique_ptr<ITradingAgent> operator()(const ValueParams& params) const {
      return std::make_unique<ValueAgent>(params);
    }
    std::unique_ptr<ITradingAgent> operator()(const NoiseParams& params) const {
      return std::make_unique<NoiseAgent>(params);
    }
    std::unique_ptr<ITradingAgent> operator()(const ModelDirectedParams& params) const {
      return std::make_unique<DirectedMaker>(params);
    }
  };
  return std::visit(Factory{}, policy);
}

} // namespace markettwin
