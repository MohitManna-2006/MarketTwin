#include "markettwin/common/hash.hpp"

#include "codec.hpp"

namespace markettwin::codec {
namespace {

constexpr std::array<std::string_view, 2> kWakeups{"Fixed", "Exponential"};
constexpr std::array<std::string_view, 3> kPhases{"Instant", "Begin", "End"};

Json maker(const MarketMakerParams& value) {
  return {
      {"half_spread_ticks", value.half_spread_ticks},
      {"quote_size", value.quote_size.value()},
      {"levels", value.levels},
      {"level_spacing_ticks", value.level_spacing_ticks},
  };
}

MarketMakerParams read_maker(const Json& value) {
  return {
      .half_spread_ticks = unsigned_integer(value.at("half_spread_ticks")),
      .quote_size = Quantity{unsigned_integer(value.at("quote_size"))},
      .levels = size_integer(value.at("levels")),
      .level_spacing_ticks = unsigned_integer(value.at("level_spacing_ticks")),
  };
}

struct PolicyWriter {
  Json operator()(const MarketMakerParams& value) const { return maker(value); }
  Json operator()(const InventoryMarketMakerParams& value) const {
    auto output = maker(value.quoting);
    output.update({
        {"risk_aversion_ppm", value.risk_aversion_ppm},
        {"volatility_alpha_ppm", value.volatility_alpha_ppm},
        {"volatility_floor_milliticks", value.volatility_floor_milliticks},
        {"max_skew_ticks", value.max_skew_ticks},
    });
    return output;
  }
  Json operator()(const MomentumParams& value) const {
    return {
        {"short_window", value.short_window},
        {"long_window", value.long_window},
        {"threshold_milliticks", value.threshold_milliticks},
        {"order_size", value.order_size.value()},
        {"slippage_ticks", value.slippage_ticks},
        {"cooldown_ns", value.cooldown.value()},
    };
  }
  Json operator()(const ValueParams& value) const {
    return {
        {"estimate_noise_milliticks", value.estimate_noise_milliticks},
        {"threshold_milliticks", value.threshold_milliticks},
        {"order_size", value.order_size.value()},
        {"slippage_ticks", value.slippage_ticks},
    };
  }
  Json operator()(const NoiseParams& value) const {
    return {
        {"buy_probability_ppm", value.buy_probability_ppm},
        {"market_order_ppm", value.market_order_ppm},
        {"min_quantity", value.min_quantity.value()},
        {"max_quantity", value.max_quantity.value()},
        {"limit_offset_ticks", value.limit_offset_ticks},
        {"max_resting_orders", value.max_resting_orders},
    };
  }
};

Json agent(const AgentConfig& value) {
  return {
      {"id", value.id.value()},
      {
          "wakeup",
          {
              {"first_ns", value.wakeup.first.value()},
              {"interval_ns", value.wakeup.interval.value()},
              {"distribution", enum_name(value.wakeup.distribution, kWakeups)},
          },
      },
      {
          "latency",
          {
              {"order_entry_ns", value.latency.order_entry.value()},
              {"order_entry_jitter_ns", value.latency.order_entry_jitter.value()},
              {"report_ns", value.latency.report.value()},
              {"market_data_ns", value.latency.market_data.value()},
          },
      },
      {
          "limits",
          {
              {"max_position", value.limits.max_position},
              {"max_order_quantity", value.limits.max_order_quantity.value()},
              {"max_open_orders", value.limits.max_open_orders},
              {"max_messages", value.limits.max_messages},
          },
      },
      {"policy", policy(value.policy)},
  };
}

AgentConfig read_agent(const Json& value) {
  fields(value, {"id", "wakeup", "latency", "limits", "policy"});
  const auto& wakeup = value.at("wakeup");
  const auto& latency = value.at("latency");
  const auto& limits = value.at("limits");
  fields(wakeup, {"first_ns", "interval_ns", "distribution"});
  fields(latency, {"order_entry_ns", "order_entry_jitter_ns", "report_ns", "market_data_ns"});
  fields(limits, {"max_position", "max_order_quantity", "max_open_orders", "max_messages"});
  return {
      .id = AgentId{unsigned_integer(value.at("id"))},
      .wakeup =
          {
              .first = SimTime{unsigned_integer(wakeup.at("first_ns"))},
              .interval = SimTime{unsigned_integer(wakeup.at("interval_ns"))},
              .distribution = read_enum<WakeupDistribution>(wakeup.at("distribution"), kWakeups),
          },
      .latency =
          {
              .order_entry = SimTime{unsigned_integer(latency.at("order_entry_ns"))},
              .order_entry_jitter = SimTime{unsigned_integer(latency.at("order_entry_jitter_ns"))},
              .report = SimTime{unsigned_integer(latency.at("report_ns"))},
              .market_data = SimTime{unsigned_integer(latency.at("market_data_ns"))},
          },
      .limits =
          {
              .max_position = unsigned_integer(limits.at("max_position")),
              .max_order_quantity = Quantity{unsigned_integer(limits.at("max_order_quantity"))},
              .max_open_orders = size_integer(limits.at("max_open_orders")),
              .max_messages = unsigned_integer(limits.at("max_messages")),
          },
      .policy = read_policy(value.at("policy")),
  };
}

Json ids(const std::vector<AgentId>& agents) {
  auto output = Json::array();
  for (const auto id : agents) {
    output.push_back(id.value());
  }
  return output;
}

std::vector<AgentId> read_ids(const Json& value) {
  if (!value.is_array()) {
    invalid("shock agents must be an array");
  }
  std::vector<AgentId> result;
  for (const auto& id : value) {
    result.emplace_back(unsigned_integer(id));
  }
  return result;
}

struct ShockWriter {
  Json operator()(const FundamentalJump& jump) const {
    return {
        {"kind", "FundamentalJump"},
        {"at_ns", jump.at.value()},
        {"delta_milliticks", jump.delta_milliticks},
    };
  }
  Json operator()(const LiquidityWithdrawal& withdrawal) const {
    return {
        {"kind", "LiquidityWithdrawal"},
        {"start_ns", withdrawal.start.value()},
        {"end_ns", withdrawal.end.value()},
        {"agents", ids(withdrawal.agents)},
        {"quote_size_ppm", withdrawal.quote_size_ppm},
    };
  }
  Json operator()(const VolatilityRegime& regime) const {
    return {
        {"kind", "VolatilityRegime"},
        {"start_ns", regime.start.value()},
        {"end_ns", regime.end.value()},
        {"volatility_ppm", regime.volatility_ppm},
    };
  }
  Json operator()(const OrderFlowImbalance& imbalance) const {
    return {
        {"kind", "OrderFlowImbalance"},
        {"start_ns", imbalance.start.value()},
        {"end_ns", imbalance.end.value()},
        {"agents", ids(imbalance.agents)},
        {"buy_probability_ppm", imbalance.buy_probability_ppm},
        {"market_order_ppm", imbalance.market_order_ppm},
        {"arrival_rate_ppm", imbalance.arrival_rate_ppm},
    };
  }
  Json operator()(const LatencyDegradation& degradation) const {
    return {
        {"kind", "LatencyDegradation"},
        {"start_ns", degradation.start.value()},
        {"end_ns", degradation.end.value()},
        {"agents", ids(degradation.agents)},
        {"added_order_entry_ns", degradation.added_order_entry.value()},
        {"added_report_ns", degradation.added_report.value()},
        {"added_market_data_ns", degradation.added_market_data.value()},
    };
  }
  Json operator()(const QuoteWithdrawalStress& stress) const {
    return {
        {"kind", "QuoteWithdrawalStress"},
        {"at_ns", stress.at.value()},
        {"agents", ids(stress.agents)},
    };
  }
};

Json shock(const Shock& value) {
  return std::visit(ShockWriter{}, value);
}

SimTime time(const Json& value, const char* field) {
  return SimTime{unsigned_integer(value.at(field))};
}

Shock read_shock(const Json& value) {
  if (!value.is_object() || !value.contains("kind")) {
    invalid("shock requires kind");
  }
  const auto kind = text(value.at("kind"));
  if (kind == "FundamentalJump") {
    fields(value, {"kind", "at_ns", "delta_milliticks"});
    return FundamentalJump{
        .at = time(value, "at_ns"),
        .delta_milliticks = signed_integer(value.at("delta_milliticks")),
    };
  }
  if (kind == "LiquidityWithdrawal") {
    fields(value, {"kind", "start_ns", "end_ns", "agents", "quote_size_ppm"});
    return LiquidityWithdrawal{
        .start = time(value, "start_ns"),
        .end = time(value, "end_ns"),
        .agents = read_ids(value.at("agents")),
        .quote_size_ppm = unsigned_integer(value.at("quote_size_ppm")),
    };
  }
  if (kind == "VolatilityRegime") {
    fields(value, {"kind", "start_ns", "end_ns", "volatility_ppm"});
    return VolatilityRegime{
        .start = time(value, "start_ns"),
        .end = time(value, "end_ns"),
        .volatility_ppm = unsigned_integer(value.at("volatility_ppm")),
    };
  }
  if (kind == "OrderFlowImbalance") {
    fields(value, {
                      "kind",
                      "start_ns",
                      "end_ns",
                      "agents",
                      "buy_probability_ppm",
                      "market_order_ppm",
                      "arrival_rate_ppm",
                  });
    return OrderFlowImbalance{
        .start = time(value, "start_ns"),
        .end = time(value, "end_ns"),
        .agents = read_ids(value.at("agents")),
        .buy_probability_ppm = unsigned_integer(value.at("buy_probability_ppm")),
        .market_order_ppm = unsigned_integer(value.at("market_order_ppm")),
        .arrival_rate_ppm = unsigned_integer(value.at("arrival_rate_ppm")),
    };
  }
  if (kind == "LatencyDegradation") {
    fields(value, {
                      "kind",
                      "start_ns",
                      "end_ns",
                      "agents",
                      "added_order_entry_ns",
                      "added_report_ns",
                      "added_market_data_ns",
                  });
    return LatencyDegradation{
        .start = time(value, "start_ns"),
        .end = time(value, "end_ns"),
        .agents = read_ids(value.at("agents")),
        .added_order_entry = time(value, "added_order_entry_ns"),
        .added_report = time(value, "added_report_ns"),
        .added_market_data = time(value, "added_market_data_ns"),
    };
  }
  if (kind == "QuoteWithdrawalStress") {
    fields(value, {"kind", "at_ns", "agents"});
    return QuoteWithdrawalStress{
        .at = time(value, "at_ns"),
        .agents = read_ids(value.at("agents")),
    };
  }
  invalid("unknown shock kind: " + kind);
}

Json words(const Rng::State& state) {
  auto output = Json::array();
  for (const auto word : state) {
    output.push_back(word);
  }
  return output;
}

} // namespace

Json policy(const PolicyConfig& value) {
  auto output = std::visit(PolicyWriter{}, value);
  output.update({{"kind", policy_name(value)}});
  return output;
}

PolicyConfig read_policy(const Json& value) {
  if (!value.is_object() || !value.contains("kind")) {
    invalid("agent policy requires kind");
  }
  const auto kind = text(value.at("kind"));
  if (kind == "MarketMaker") {
    fields(value, {"kind", "half_spread_ticks", "quote_size", "levels", "level_spacing_ticks"});
    return read_maker(value);
  }
  if (kind == "InventoryMarketMaker") {
    fields(value, {
                      "kind",
                      "half_spread_ticks",
                      "quote_size",
                      "levels",
                      "level_spacing_ticks",
                      "risk_aversion_ppm",
                      "volatility_alpha_ppm",
                      "volatility_floor_milliticks",
                      "max_skew_ticks",
                  });
    return InventoryMarketMakerParams{
        .quoting = read_maker(value),
        .risk_aversion_ppm = unsigned_integer(value.at("risk_aversion_ppm")),
        .volatility_alpha_ppm = unsigned_integer(value.at("volatility_alpha_ppm")),
        .volatility_floor_milliticks = unsigned_integer(value.at("volatility_floor_milliticks")),
        .max_skew_ticks = unsigned_integer(value.at("max_skew_ticks")),
    };
  }
  if (kind == "Momentum") {
    fields(value, {
                      "kind",
                      "short_window",
                      "long_window",
                      "threshold_milliticks",
                      "order_size",
                      "slippage_ticks",
                      "cooldown_ns",
                  });
    return MomentumParams{
        .short_window = size_integer(value.at("short_window")),
        .long_window = size_integer(value.at("long_window")),
        .threshold_milliticks = unsigned_integer(value.at("threshold_milliticks")),
        .order_size = Quantity{unsigned_integer(value.at("order_size"))},
        .slippage_ticks = unsigned_integer(value.at("slippage_ticks")),
        .cooldown = SimTime{unsigned_integer(value.at("cooldown_ns"))},
    };
  }
  if (kind == "Value") {
    fields(value, {
                      "kind",
                      "estimate_noise_milliticks",
                      "threshold_milliticks",
                      "order_size",
                      "slippage_ticks",
                  });
    return ValueParams{
        .estimate_noise_milliticks = unsigned_integer(value.at("estimate_noise_milliticks")),
        .threshold_milliticks = unsigned_integer(value.at("threshold_milliticks")),
        .order_size = Quantity{unsigned_integer(value.at("order_size"))},
        .slippage_ticks = unsigned_integer(value.at("slippage_ticks")),
    };
  }
  if (kind == "Noise") {
    fields(value, {
                      "kind",
                      "buy_probability_ppm",
                      "market_order_ppm",
                      "min_quantity",
                      "max_quantity",
                      "limit_offset_ticks",
                      "max_resting_orders",
                  });
    return NoiseParams{
        .buy_probability_ppm = unsigned_integer(value.at("buy_probability_ppm")),
        .market_order_ppm = unsigned_integer(value.at("market_order_ppm")),
        .min_quantity = Quantity{unsigned_integer(value.at("min_quantity"))},
        .max_quantity = Quantity{unsigned_integer(value.at("max_quantity"))},
        .limit_offset_ticks = unsigned_integer(value.at("limit_offset_ticks")),
        .max_resting_orders = size_integer(value.at("max_resting_orders")),
    };
  }
  invalid("unknown agent policy kind: " + kind);
}

Json scenario(const ScenarioConfig& value) {
  auto agents = Json::array();
  for (const auto& item : value.agents) {
    agents.push_back(agent(item));
  }
  auto shocks = Json::array();
  for (const auto& item : value.shocks) {
    shocks.push_back(shock(item));
  }
  return {
      {"reference_price_ticks", value.reference_price.value()},
      {
          "fundamental",
          {
              {"initial_milliticks", value.fundamental.initial_milliticks},
              {"update_interval_ns", value.fundamental.update_interval.value()},
              {"volatility_milliticks", value.fundamental.volatility_milliticks},
              {"mean_reversion_ppm", value.fundamental.mean_reversion_ppm},
          },
      },
      {
          "sampling",
          value.sampling ? Json{
                               {"interval_ns", value.sampling->interval.value()},
                               {"depth", value.sampling->depth},
                           }
                         : Json(nullptr),
      },
      {"agents", agents},
      {"shocks", shocks},
  };
}

ScenarioConfig read_scenario(const Json& value) {
  fields(value, {"reference_price_ticks", "fundamental", "sampling", "agents", "shocks"});
  const auto& fundamental = value.at("fundamental");
  fields(fundamental, {
                          "initial_milliticks",
                          "update_interval_ns",
                          "volatility_milliticks",
                          "mean_reversion_ppm",
                      });
  ScenarioConfig result{
      .reference_price = PriceTicks{signed_integer(value.at("reference_price_ticks"))},
      .fundamental =
          {
              .initial_milliticks = signed_integer(fundamental.at("initial_milliticks")),
              .update_interval = SimTime{unsigned_integer(fundamental.at("update_interval_ns"))},
              .volatility_milliticks = unsigned_integer(fundamental.at("volatility_milliticks")),
              .mean_reversion_ppm = unsigned_integer(fundamental.at("mean_reversion_ppm")),
          },
      .sampling = {},
      .agents = {},
      .shocks = {},
  };
  if (const auto& sampling = value.at("sampling"); !sampling.is_null()) {
    fields(sampling, {"interval_ns", "depth"});
    result.sampling = SamplingConfig{
        .interval = SimTime{unsigned_integer(sampling.at("interval_ns"))},
        .depth = size_integer(sampling.at("depth")),
    };
  }
  if (!value.at("agents").is_array() || !value.at("shocks").is_array()) {
    invalid("scenario agents and shocks must be arrays");
  }
  for (const auto& item : value.at("agents")) {
    result.agents.push_back(read_agent(item));
  }
  for (const auto& item : value.at("shocks")) {
    result.shocks.push_back(read_shock(item));
  }
  return result;
}

Json shock_transition(std::size_t index, ShockPhase phase, const Shock& value) {
  return {{"index", index}, {"phase", enum_name(phase, kPhases)}, {"shock", shock(value)}};
}

Json fundamental_update(const FundamentalProcess& value) {
  return {
      {"value_milliticks", value.value()},
      {"anchor_milliticks", value.anchor()},
      {"volatility_milliticks", value.volatility_milliticks()},
  };
}

Json scenario_summary(const ScenarioSummary& value) {
  auto agents = Json::array();
  for (const auto& item : value.agents) {
    agents.push_back({
        {"id", item.id.value()},
        {"policy", item.policy},
        {"wakeups", item.wakeups},
        {"orders_sent", item.orders_sent},
        {"cancels_sent", item.cancels_sent},
        {"intents_dropped", item.intents_dropped},
        {"fills", item.fills},
        {"filled_quantity", item.filled_quantity.value()},
        {"net_position", item.net_position},
    });
  }
  return {
      {"fundamental_milliticks", value.fundamental_milliticks},
      {"fundamental_updates", value.fundamental_updates},
      {"market_data_updates", value.market_data_updates},
      {"shock_transitions", value.shock_transitions},
      {"rng_algorithm", kRngAlgorithm},
      {"rng_digest", value.rng_digest},
      {"agents", agents},
  };
}

std::string rng_digest(const Rng::State& fundamental, const std::vector<Rng::State>& agents) {
  auto streams = Json::array();
  for (const auto& state : agents) {
    streams.push_back(words(state));
  }
  return sha256(Json{{"fundamental", words(fundamental)}, {"agents", streams}}.dump());
}

} // namespace markettwin::codec
