#include "markettwin/common/run_error.hpp"

#include "../../test_support.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <limits>
#include <nlohmann/json.hpp>

namespace markettwin {
namespace {
using Json = nlohmann::json;

TEST(ConfigTest, CanonicalRoundTripAndRegistrationOrderAreStable) {
  auto config = test::fixture();
  config.seed = std::numeric_limits<std::uint64_t>::max();
  config.commands.at(1).command =
      test::order(std::numeric_limits<std::uint64_t>::max(), Side::Sell);
  const auto bytes = canonical_config(config);
  EXPECT_EQ(canonical_config(parse_config(bytes)), bytes);
  std::ranges::reverse(config.exchange.agents);
  EXPECT_EQ(canonical_config(config), bytes);
  EXPECT_EQ(Simulation{parse_config(bytes)}.run().seed, std::numeric_limits<std::uint64_t>::max());
}

TEST(ConfigTest, RejectsMalformedNumericInputsEnumsAndUnknownFields) {
  const auto valid = Json::parse(canonical_config(test::fixture()));
  const auto reject = [](const Json& value) {
    EXPECT_THROW(static_cast<void>(parse_config(value.dump())), RunError);
  };
  for (const Json& bad : {Json(-1), Json(1.5), Json(true), Json("12"), Json(nullptr)}) {
    auto invalid = valid;
    invalid.at("seed") = bad;
    reject(invalid);
  }
  auto invalid = valid;
  invalid.at("commands").at(1).at("command").at("limit_price_ticks") =
      std::numeric_limits<std::uint64_t>::max();
  reject(invalid);
  invalid = valid;
  invalid.at("commands").at(1).at("command").at("side") = "Bid";
  reject(invalid);
  invalid = valid;
  invalid.update({{"unknown", 1}});
  reject(invalid);
  invalid = valid;
  invalid.erase("run_id");
  reject(invalid);
  invalid = valid;
  invalid.at("commands").at(1).at("command").update({{"arrival_sequence", 1}});
  reject(invalid);
  invalid = valid;
  invalid.at("commands").at(0).at("command").at("depth") = 0;
  reject(invalid);
  invalid = valid;
  invalid.at("schema_version") = 2;
  reject(invalid);
  invalid = valid;
  invalid.at("run_id") = "../run";
  reject(invalid);
  invalid = valid;
  invalid.at("max_queue") = 1;
  reject(invalid);
}

TEST(ConfigTest, RejectsDuplicateKeysHugeIntegersTrailingDataAndExcessiveNesting) {
  auto input = canonical_config(test::fixture());
  input.insert(1, "\"seed\":1,");
  EXPECT_THROW(static_cast<void>(parse_config(input)), RunError);
  input = canonical_config(test::fixture());
  const auto seed = input.find("\"seed\":42");
  ASSERT_NE(seed, std::string::npos);
  input.replace(seed, 9, "\"seed\":18446744073709551616");
  EXPECT_THROW(static_cast<void>(parse_config(input)), RunError);
  EXPECT_THROW(static_cast<void>(parse_config(canonical_config(test::fixture()) + "{}")), RunError);
  EXPECT_THROW(static_cast<void>(parse_config(std::string(40, '[') + "0" + std::string(40, ']'))),
               RunError);
}

TEST(ConfigTest, NativeCallersCannotBypassBoundaryValidation) {
  auto config = test::fixture();
  std::get<Order>(config.commands.at(1).command).arrival_time = SimTime{1};
  EXPECT_THROW(Simulation{config}, RunError);
  config = test::fixture();
  std::get<Order>(config.commands.at(1).command).remaining_quantity = Quantity{4};
  EXPECT_THROW(Simulation{config}, RunError);
  config = test::fixture();
  config.exchange.agents = {AgentId{1}, AgentId{1}};
  EXPECT_THROW(Simulation{config}, RunError);
  config = test::fixture();
  config.max_trace_bytes = 0;
  EXPECT_THROW(Simulation{config}, RunError);
}

TEST(ConfigTest, DomainInvalidOrdersBecomeLoggedExchangeRejections) {
  auto config = test::fixture();
  auto invalid = test::order(9, Side::Buy, 99, 0);
  invalid.limit_price = PriceTicks{-1};
  config.commands = {test::at(0, invalid)};
  const auto summary = Simulation{parse_config(canonical_config(config))}.run();
  EXPECT_EQ(summary.event_count, 1U);
  EXPECT_EQ(summary.trade_count, 0U);
  EXPECT_EQ(summary.resting_order_count, 0U);
}

} // namespace
} // namespace markettwin
