#include "markettwin/common/run_error.hpp"

#include "../../test_support.hpp"

#include <gtest/gtest.h>

namespace markettwin {
namespace {
using Json = nlohmann::json;

RunConfig with_shocks() {
  auto config = test::scenario_fixture();
  test::scenario(config).shocks = {
      FundamentalJump{.at = SimTime{10}, .delta_milliticks = -5'000},
      LiquidityWithdrawal{
          .start = SimTime{20},
          .end = SimTime{30},
          .agents = {AgentId{1}, AgentId{2}},
          .quote_size_ppm = 250'000,
      },
      VolatilityRegime{.start = SimTime{5}, .end = SimTime{50}, .volatility_ppm = 3'000'000},
      OrderFlowImbalance{
          .start = SimTime{10},
          .end = SimTime{40},
          .agents = {AgentId{5}, AgentId{6}},
          .buy_probability_ppm = 800'000,
          .market_order_ppm = 600'000,
          .arrival_rate_ppm = 2'000'000,
      },
      LatencyDegradation{
          .start = SimTime{15},
          .end = SimTime{35},
          .agents = {AgentId{3}, AgentId{4}},
          .added_order_entry = SimTime{5},
          .added_report = SimTime{0},
          .added_market_data = SimTime{7},
      },
      QuoteWithdrawalStress{.at = SimTime{25}, .agents = {AgentId{1}, AgentId{6}}},
  };
  return config;
}

TEST(ScenarioConfigTest, RoundTripsEveryPolicyAndShockThroughCanonicalVersionTwo) {
  const auto config = with_shocks();
  const auto bytes = canonical_config(config);
  const auto parsed = parse_config(bytes);
  EXPECT_EQ(parsed.scenario, config.scenario);
  EXPECT_EQ(canonical_config(parsed), bytes);
  EXPECT_EQ(Json::parse(bytes).at("schema_version"), 2);

  auto without_sampling = config;
  test::scenario(without_sampling).sampling.reset();
  const auto sparse = canonical_config(without_sampling);
  EXPECT_TRUE(Json::parse(sparse).at("scenario").at("sampling").is_null());
  EXPECT_EQ(parse_config(sparse).scenario, without_sampling.scenario);
}

TEST(ScenarioConfigTest, ScriptedConfigsKeepVersionOneAndVersionsDoNotMix) {
  const auto scripted = Json::parse(canonical_config(test::fixture()));
  EXPECT_EQ(scripted.at("schema_version"), 1);
  EXPECT_FALSE(scripted.contains("scenario"));
  const auto scenario = Json::parse(canonical_config(test::scenario_fixture()));

  auto mixed = scripted;
  mixed.update({{"scenario", scenario.at("scenario")}});
  EXPECT_THROW(static_cast<void>(parse_config(mixed.dump())), RunError);
  mixed = scenario;
  mixed.erase("scenario");
  EXPECT_THROW(static_cast<void>(parse_config(mixed.dump())), RunError);
  mixed = scenario;
  mixed.at("schema_version") = 3;
  EXPECT_THROW(static_cast<void>(parse_config(mixed.dump())), RunError);
}

TEST(ScenarioConfigTest, RejectsMalformedScenarioJson) {
  const auto valid = Json::parse(canonical_config(with_shocks()));
  const auto reject = [&](const std::function<void(Json&)>& mutate) {
    auto invalid = valid;
    mutate(invalid.at("scenario"));
    EXPECT_THROW(static_cast<void>(parse_config(invalid.dump())), RunError) << invalid.dump();
  };
  reject([](Json& value) { value.update({{"unknown", 1}}); });
  reject([](Json& value) { value.erase("fundamental"); });
  reject([](Json& value) { value.at("fundamental").at("volatility_milliticks") = 1.5; });
  reject([](Json& value) { value.at("fundamental").at("volatility_milliticks") = -1; });
  reject([](Json& value) { value.at("agents").at(0).at("policy").at("kind") = "Arbitrage"; });
  reject([](Json& value) { value.at("agents").at(0).at("policy").erase("levels"); });
  reject([](Json& value) { value.at("agents").at(0).at("policy").update({{"extra", 1}}); });
  reject([](Json& value) { value.at("agents").at(0).at("wakeup").at("distribution") = "Poisson"; });
  reject([](Json& value) { value.at("agents").at(0).at("latency").erase("report_ns"); });
  reject([](Json& value) { value.at("agents").at(0).at("limits").at("max_position") = "many"; });
  reject([](Json& value) { value.at("shocks").at(0).at("kind") = "Earthquake"; });
  reject([](Json& value) { value.at("shocks").at(1).at("agents") = 1; });
  reject([](Json& value) { value.at("shocks").at(3).erase("arrival_rate_ppm"); });
  reject([](Json& value) { value.at("shocks").at(4).at("added_report_ns") = -1; });
  reject([](Json& value) { value.at("shocks").at(5).at("agents") = Json::array({"one"}); });
  reject([](Json& value) { value.at("agents") = Json::object(); });
}

TEST(ScenarioConfigTest, RejectsOutOfRangeScenarioValuesForNativeCallersToo) {
  const auto reject = [](const std::function<void(RunConfig&)>& mutate) {
    auto config = with_shocks();
    mutate(config);
    EXPECT_THROW(validate_config(config), RunError);
  };
  const auto agent = [](RunConfig& config, std::size_t slot) -> AgentConfig& {
    return test::scenario(config).agents.at(slot);
  };
  validate_config(with_shocks());
  reject([](RunConfig& config) { test::scenario(config).reference_price = PriceTicks{0}; });
  reject([](RunConfig& config) { test::scenario(config).fundamental.initial_milliticks = 999; });
  reject(
      [](RunConfig& config) { test::scenario(config).fundamental.update_interval = SimTime{0}; });
  reject(
      [](RunConfig& config) { test::scenario(config).fundamental.mean_reversion_ppm = 1'000'001; });
  reject([](RunConfig& config) { test::scenario(config).sampling->depth = 0; });
  reject([](RunConfig& config) { test::scenario(config).sampling->interval = SimTime{0}; });
  reject([](RunConfig& config) { config.limits.horizon = SimTime{2'000'000'000'000'000'000}; });
  // Agent identity, timing, and limits.
  reject([&](RunConfig& config) { agent(config, 1).id = AgentId{1}; });
  reject([&](RunConfig& config) { agent(config, 0).id = AgentId{99}; });
  reject([&](RunConfig& config) { agent(config, 0).wakeup.interval = SimTime{0}; });
  reject(
      [&](RunConfig& config) { agent(config, 0).latency.report = SimTime{2'000'000'000'000'000}; });
  reject([&](RunConfig& config) { agent(config, 0).limits.max_position = 0; });
  reject([&](RunConfig& config) { agent(config, 0).limits.max_order_quantity = Quantity{0}; });
  reject([&](RunConfig& config) { agent(config, 0).limits.max_open_orders = 0; });
  reject([&](RunConfig& config) { agent(config, 0).limits.max_messages = 0; });
  // Policy parameters.
  reject([&](RunConfig& config) {
    std::get<MarketMakerParams>(agent(config, 0).policy).half_spread_ticks = 0;
  });
  reject([&](RunConfig& config) {
    std::get<MarketMakerParams>(agent(config, 0).policy).levels = 9; // 18 quotes > 16 open orders
  });
  reject([&](RunConfig& config) {
    std::get<MarketMakerParams>(agent(config, 0).policy).quote_size = Quantity{101};
  });
  reject([&](RunConfig& config) {
    std::get<InventoryMarketMakerParams>(agent(config, 1).policy).risk_aversion_ppm = 2'000'000;
  });
  reject([&](RunConfig& config) {
    std::get<MomentumParams>(agent(config, 3).policy).short_window = 5; // not below long
  });
  reject([&](RunConfig& config) {
    std::get<ValueParams>(agent(config, 2).policy).order_size = Quantity{0};
  });
  reject([&](RunConfig& config) {
    std::get<NoiseParams>(agent(config, 4).policy).min_quantity = Quantity{21};
  });
  reject([&](RunConfig& config) {
    std::get<NoiseParams>(agent(config, 4).policy).buy_probability_ppm = 1'000'001;
  });
  reject([&](RunConfig& config) {
    std::get<NoiseParams>(agent(config, 4).policy).max_resting_orders = 16;
  });
  // Shocks.
  reject([](RunConfig& config) {
    std::get<FundamentalJump>(test::scenario(config).shocks.at(0)).delta_milliticks = 0;
  });
  reject([](RunConfig& config) {
    std::get<LiquidityWithdrawal>(test::scenario(config).shocks.at(1)).end = SimTime{20};
  });
  reject([](RunConfig& config) {
    // Agent 5 is a noise trader.
    std::get<LiquidityWithdrawal>(test::scenario(config).shocks.at(1)).agents = {AgentId{5}};
  });
  reject([](RunConfig& config) {
    std::get<LiquidityWithdrawal>(test::scenario(config).shocks.at(1)).quote_size_ppm = 1'000'001;
  });
  reject([](RunConfig& config) {
    test::scenario(config).shocks.emplace_back(LiquidityWithdrawal{
        .start = SimTime{29},
        .end = SimTime{40},
        .agents = {AgentId{2}},
        .quote_size_ppm = 0,
    });
  });
  reject([](RunConfig& config) {
    test::scenario(config).shocks.emplace_back(
        VolatilityRegime{.start = SimTime{49}, .end = SimTime{60}, .volatility_ppm = kPpm});
  });
  const auto imbalance = [](RunConfig& config) -> OrderFlowImbalance& {
    return std::get<OrderFlowImbalance>(test::scenario(config).shocks.at(3));
  };
  const auto degradation = [](RunConfig& config) -> LatencyDegradation& {
    return std::get<LatencyDegradation>(test::scenario(config).shocks.at(4));
  };
  const auto stress = [](RunConfig& config) -> QuoteWithdrawalStress& {
    return std::get<QuoteWithdrawalStress>(test::scenario(config).shocks.at(5));
  };
  reject([&](RunConfig& config) { imbalance(config).agents = {AgentId{1}}; }); // a maker
  reject([&](RunConfig& config) { imbalance(config).agents = {}; });
  reject([&](RunConfig& config) { imbalance(config).agents = {AgentId{5}, AgentId{5}}; });
  reject([&](RunConfig& config) { imbalance(config).buy_probability_ppm = 1'000'001; });
  reject([&](RunConfig& config) { imbalance(config).market_order_ppm = 1'000'001; });
  reject([&](RunConfig& config) { imbalance(config).arrival_rate_ppm = 9'999; });
  reject([&](RunConfig& config) { imbalance(config).arrival_rate_ppm = 100'000'001; });
  reject([&](RunConfig& config) { imbalance(config).end = SimTime{10}; });
  reject([](RunConfig& config) {
    test::scenario(config).shocks.emplace_back(OrderFlowImbalance{
        .start = SimTime{39},
        .end = SimTime{45},
        .agents = {AgentId{6}},
    });
  });
  reject([&](RunConfig& config) {
    degradation(config).added_order_entry = SimTime{0};
    degradation(config).added_market_data = SimTime{0};
  });
  reject([&](RunConfig& config) {
    degradation(config).added_report = SimTime{2'000'000'000'000'000};
  });
  reject([&](RunConfig& config) { degradation(config).agents = {AgentId{99}}; });
  reject([](RunConfig& config) {
    test::scenario(config).shocks.emplace_back(LatencyDegradation{
        .start = SimTime{0},
        .end = SimTime{16},
        .agents = {AgentId{4}},
        .added_order_entry = SimTime{1},
    });
  });
  reject([&](RunConfig& config) { stress(config).agents = {}; });
  reject([&](RunConfig& config) { stress(config).agents = {AgentId{1}, AgentId{1}}; });
  reject([&](RunConfig& config) { stress(config).at = SimTime{2'000'000'000'000'000'000}; });
  // Scripted orders may not enter the agents' order-ID space.
  reject([](RunConfig& config) {
    config.commands = {test::at(0, test::order(1ULL << 32U, Side::Buy))};
  });
}

TEST(ScenarioConfigTest, AdjacentShockIntervalsAndDisjointAgentsAreAccepted) {
  auto config = with_shocks();
  test::scenario(config).shocks.emplace_back(LiquidityWithdrawal{
      .start = SimTime{30},
      .end = SimTime{40},
      .agents = {AgentId{2}},
      .quote_size_ppm = 0,
  });
  test::scenario(config).shocks.emplace_back(
      VolatilityRegime{.start = SimTime{50}, .end = SimTime{60}, .volatility_ppm = kPpm});
  test::scenario(config).shocks.emplace_back(OrderFlowImbalance{
      .start = SimTime{40},
      .end = SimTime{50},
      .agents = {AgentId{5}},
  });
  test::scenario(config).shocks.emplace_back(LatencyDegradation{
      .start = SimTime{0},
      .end = SimTime{15},
      .agents = {AgentId{3}},
      .added_report = SimTime{1},
  });
  // Instants may repeat; they have no extent to overlap.
  test::scenario(config).shocks.emplace_back(
      QuoteWithdrawalStress{.at = SimTime{25}, .agents = {AgentId{1}}});
  EXPECT_NO_THROW(validate_config(config));
}

} // namespace
} // namespace markettwin
