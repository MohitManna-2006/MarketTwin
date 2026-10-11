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

void check_shocks(const ScenarioConfig& scenario) {
  require(scenario.shocks.size() <= kMaxShocks, "too many shocks");
  std::vector<Interval> regimes;
  std::vector<std::pair<AgentId, Interval>> withdrawals;
  for (const auto& shock : scenario.shocks) {
    if (const auto* jump = std::get_if<FundamentalJump>(&shock)) {
      require(jump->at.value() <= kMaxHorizon && jump->delta_milliticks != 0 &&
                  jump->delta_milliticks >= -to_signed(kMaxMilliticks) &&
                  jump->delta_milliticks <= to_signed(kMaxMilliticks),
              "fundamental jump must be nonzero and within 1e12 milliticks");
    } else if (const auto* withdrawal = std::get_if<LiquidityWithdrawal>(&shock)) {
      check_interval(withdrawal->start, withdrawal->end);
      require(withdrawal->quote_size_ppm <= kPpm && !withdrawal->agents.empty(),
              "liquidity withdrawal needs agents and a quote size of at most 1e6 ppm");
      for (const auto id : withdrawal->agents) {
        const auto found = std::ranges::find(scenario.agents, id, &AgentConfig::id);
        require(found != scenario.agents.end() &&
                    (std::holds_alternative<MarketMakerParams>(found->policy) ||
                     std::holds_alternative<InventoryMarketMakerParams>(found->policy)),
                "liquidity withdrawal may only name market-maker agents");
        withdrawals.emplace_back(id, Interval{.start = withdrawal->start, .end = withdrawal->end});
      }
    } else {
      const auto& regime = std::get<VolatilityRegime>(shock);
      check_interval(regime.start, regime.end);
      require(regime.volatility_ppm <= kMaxVolatilityPpm, "volatility regime exceeds 100x");
      regimes.push_back({.start = regime.start, .end = regime.end});
    }
  }
  // Overlap would need a combination rule; require the config to be unambiguous instead.
  require_disjoint(regimes, "volatility regimes must not overlap");
  std::ranges::sort(withdrawals, {}, [](const auto& item) { return item.first; });
  for (auto first = withdrawals.begin(); first != withdrawals.end();) {
    const auto last = std::find_if(first, withdrawals.end(),
                                   [&](const auto& item) { return item.first != first->first; });
    std::vector<Interval> intervals;
    for (auto item = first; item != last; ++item) {
      intervals.push_back(item->second);
    }
    require_disjoint(intervals, "liquidity withdrawals must not overlap for one agent");
    first = last;
  }
}

} // namespace

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
  constexpr std::array<std::string_view, 5> kNames{
      "MarketMaker", "InventoryMarketMaker", "Momentum", "Value", "Noise",
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
  };
  return std::visit(Factory{}, policy);
}

} // namespace markettwin
