#include "markettwin/common/run_error.hpp"
#include "markettwin/live/live.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace markettwin {
namespace {
using Json = nlohmann::json;
constexpr auto kConfig =
    R"({"provider":"alpaca","feed":"iex","stale_after_ns":15000000000,"skew_alert_ns":10000000000})";
constexpr std::int64_t kWall = 1'700'000'000'000'000'000;
constexpr std::int64_t kSecond = 1'000'000'000;

Json trade(const std::string& time, const std::string& price, std::int64_t quantity,
           std::int64_t identifier = 1) {
  return {{"T", "t"},   {"S", "AAPL"},   {"i", identifier}, {"x", "V"},
          {"p", price}, {"s", quantity}, {"t", time},       {"c", Json::array({"@", "I"})},
          {"z", "C"}};
}

Json quote(const std::string& time, const std::string& bid, std::int64_t bid_size,
           const std::string& ask, std::int64_t ask_size) {
  return {{"T", "q"},  {"S", "AAPL"}, {"bx", "V"},      {"bp", bid}, {"bs", bid_size},
          {"ax", "V"}, {"ap", ask},   {"as", ask_size}, {"t", time}, {"c", Json::array({"R"})},
          {"z", "C"}};
}

// One frame per call, received `monotonic` nanoseconds into the test.
Json batch(const std::vector<Json>& messages, std::int64_t monotonic = 0, std::uint64_t frame = 1) {
  return {{"now_wall_ns", std::to_string(kWall + monotonic)},
          {"now_monotonic_ns", monotonic},
          {"frames", Json::array({{{"frame", frame},
                                   {"receipt_wall_ns", std::to_string(kWall + monotonic)},
                                   {"receipt_monotonic_ns", monotonic},
                                   {"messages", messages}}})}};
}

Json run(live::LiveNormalizer& normalizer, const std::vector<Json>& messages,
         std::int64_t monotonic = 0) {
  return Json::parse(normalizer.process_batch(batch(messages, monotonic).dump()));
}

Json state(const live::LiveNormalizer& normalizer, std::int64_t monotonic = 0) {
  return Json::parse(normalizer.snapshot(kWall + monotonic, monotonic)).at("states").at("AAPL");
}

TEST(LiveNormalizerTest, NormalizesATradeWithExactFixedPointAndSourcePrecision) {
  live::LiveNormalizer normalizer{kConfig};
  const auto result = run(normalizer, {trade("2024-03-11T13:30:00.208Z", "126.55", 100, 96921)});
  ASSERT_EQ(result.at("events").size(), 1U);
  const auto& event = result.at("events").at(0);
  EXPECT_EQ(event.at("kind"), "trade");
  EXPECT_EQ(event.at("price_micro_usd"), 126'550'000);
  EXPECT_EQ(event.at("source_price"), "126.55");
  EXPECT_EQ(event.at("quantity"), 100);
  EXPECT_EQ(event.at("trade_id"), 96921);
  EXPECT_EQ(event.at("venue"), "V");
  EXPECT_EQ(event.at("tape"), "C");
  EXPECT_EQ(event.at("conditions"), Json::array({"@", "I"}));
  EXPECT_EQ(event.at("timestamp_ns"), "1710163800208000000");
  EXPECT_EQ(event.at("timestamp_precision_digits"), 3);
  EXPECT_EQ(event.at("source_timestamp"), "2024-03-11T13:30:00.208Z");
  EXPECT_EQ(event.at("provider"), "alpaca");
  EXPECT_EQ(event.at("feed"), "iex");
  EXPECT_EQ(event.at("receipt_wall_ns"), std::to_string(kWall));
  EXPECT_EQ(event.at("classification"), "in_order");
  EXPECT_EQ(event.at("live_sequence"), 1);
  EXPECT_TRUE(result.at("rejects").empty());
}

TEST(LiveNormalizerTest, NormalizesAQuoteAndDerivesSpreadAndMidpoint) {
  live::LiveNormalizer normalizer{kConfig};
  const auto result =
      run(normalizer, {quote("2024-03-11T13:30:00.335689322Z", "87.66", 1, "87.68", 4)});
  const auto& event = result.at("events").at(0);
  EXPECT_EQ(event.at("bid_micro_usd"), 87'660'000);
  EXPECT_EQ(event.at("ask_micro_usd"), 87'680'000);
  EXPECT_EQ(event.at("bid_size"), 1);
  EXPECT_EQ(event.at("ask_size"), 4);
  EXPECT_EQ(event.at("timestamp_precision_digits"), 9);
  EXPECT_EQ(event.at("timestamp_ns"), "1710163800335689322");
  const auto& snapshot = result.at("states").at("AAPL");
  EXPECT_EQ(snapshot.at("quote_state"), "two_sided");
  EXPECT_EQ(snapshot.at("spread_micro_usd"), 20'000);
  EXPECT_EQ(snapshot.at("midpoint_micro_usd"), 87'670'000);
  EXPECT_EQ(snapshot.at("freshness").at("spread_quality"), "current");
  EXPECT_TRUE(snapshot.at("last_trade").is_null());
}

TEST(LiveNormalizerTest, KeepsFixedPointExactWhereBinaryFloatingPointWouldNot) {
  live::LiveNormalizer normalizer{kConfig};
  const auto result =
      run(normalizer, {trade("2024-03-11T13:30:00Z", "0.1", 1, 1),
                       trade("2024-03-11T13:30:01Z", "1234567.000001", 1, 2),
                       trade("2024-03-11T13:30:02Z", "9223372036854.775807", 1, 3)});
  EXPECT_EQ(result.at("events").at(0).at("price_micro_usd"), 100'000);
  EXPECT_EQ(result.at("events").at(1).at("price_micro_usd"), 1'234'567'000'001);
  EXPECT_EQ(result.at("events").at(2).at("price_micro_usd"), 9'223'372'036'854'775'807);
}

TEST(LiveNormalizerTest, RejectsInvalidPricesSizesAndTimestampsWithReasons) {
  live::LiveNormalizer normalizer{kConfig};
  auto numeric = trade("2024-03-11T13:30:00Z", "1", 1);
  numeric["p"] = 126.55;
  auto fractional = trade("2024-03-11T13:30:00Z", "1", 1);
  fractional["s"] = 0.5;
  auto lowercase = trade("2024-03-11T13:30:00Z", "1", 1);
  lowercase["S"] = "aapl";
  const std::vector<Json> messages = {
      numeric,
      trade("2024-03-11T13:30:00Z", "1.0000001", 1),
      trade("2024-03-11T13:30:00Z", "-1", 1),
      trade("2024-03-11T13:30:00Z", "0", 1),
      trade("2024-03-11T13:30:00Z", "9223372036855", 1),
      trade("2024-03-11T13:30:00Z", "1e2", 1),
      trade("2024-03-11T13:30:00Z", "1", 0),
      fractional,
      trade("2024-03-11T09:30:00-04:00", "1", 1),
      trade("2024-02-30T13:30:00Z", "1", 1),
      trade("2024-03-11T13:30:00.1234567890Z", "1", 1),
      trade("2024-03-11 13:30:00Z", "1", 1),
      lowercase,
      quote("2024-03-11T13:30:00Z", "1", -1, "2", 1),
      quote("2024-03-11T13:30:00Z", "1", 0, "2", 1),
      Json{{"S", "AAPL"}},
      Json(7),
  };
  const auto result = run(normalizer, messages);
  EXPECT_TRUE(result.at("events").empty());
  ASSERT_EQ(result.at("rejects").size(), messages.size());
  for (const auto& reject : result.at("rejects")) {
    EXPECT_FALSE(reject.at("reason").get<std::string>().empty());
    EXPECT_FALSE(reject.at("code").get<std::string>().empty());
  }
  EXPECT_EQ(result.at("rejects").at(0).at("reason"), "missing decimal-string price: p");
  EXPECT_EQ(result.at("rejects").at(1).at("reason"), "price exceeds six decimal places: p");
  EXPECT_EQ(result.at("rejects").at(6).at("reason"), "trade quantity must be positive");
  EXPECT_EQ(result.at("rejects").at(8).at("index"), 8);
  EXPECT_EQ(result.at("rejects").at(14).at("reason"),
            "absent quote side must have zero price and size");
  // A refused record creates no instrument and changes no state.
  EXPECT_TRUE(Json::parse(normalizer.snapshot(kWall, 0)).at("states").empty());
  EXPECT_EQ(Json::parse(normalizer.snapshot(kWall, 0)).at("counters").at("rejected"),
            messages.size());
}

TEST(LiveNormalizerTest, AnAbsentQuoteSideIsNullAndNeverZero) {
  live::LiveNormalizer normalizer{kConfig};
  auto result = run(normalizer, {quote("2024-03-11T13:30:00Z", "170.10", 2, "0", 0)});
  const auto& event = result.at("events").at(0);
  EXPECT_EQ(event.at("bid_micro_usd"), 170'100'000);
  EXPECT_TRUE(event.at("ask_micro_usd").is_null());
  EXPECT_TRUE(event.at("ask_size").is_null());
  EXPECT_EQ(event.at("quote_state"), "bid_only");
  const auto& snapshot = result.at("states").at("AAPL");
  EXPECT_TRUE(snapshot.at("spread_micro_usd").is_null());
  EXPECT_TRUE(snapshot.at("midpoint_micro_usd").is_null());
  EXPECT_EQ(snapshot.at("freshness").at("spread_quality"), "unavailable");
  result = run(normalizer, {quote("2024-03-11T13:30:01Z", "0", 0, "0", 0)});
  EXPECT_EQ(result.at("states").at("AAPL").at("quote_state"), "empty");
}

TEST(LiveNormalizerTest, TradesAndQuotesNeverOverwriteEachOther) {
  live::LiveNormalizer normalizer{kConfig};
  run(normalizer, {quote("2024-03-11T13:30:00Z", "10.00", 3, "10.02", 5)});
  const auto after_trade = run(normalizer, {trade("2024-03-11T13:30:01Z", "10.01", 7)});
  const auto& with_trade = after_trade.at("states").at("AAPL");
  EXPECT_EQ(with_trade.at("quote").at("bid_micro_usd"), 10'000'000);
  EXPECT_EQ(with_trade.at("quote").at("ask_size"), 5);
  EXPECT_EQ(with_trade.at("last_trade").at("price_micro_usd"), 10'010'000);
  const auto after_quote = run(normalizer, {quote("2024-03-11T13:30:02Z", "10.03", 1, "10.04", 1)});
  const auto& with_quote = after_quote.at("states").at("AAPL");
  EXPECT_EQ(with_quote.at("last_trade").at("price_micro_usd"), 10'010'000);
  EXPECT_EQ(with_quote.at("last_trade").at("quantity"), 7);
  EXPECT_EQ(with_quote.at("quote").at("bid_micro_usd"), 10'030'000);
  EXPECT_EQ(with_quote.at("quote_trade_skew_ns"), kSecond);
}

TEST(LiveNormalizerTest, ClassifiesLockedAndCrossedQuotesWithoutAMidpointForCrossed) {
  live::LiveNormalizer normalizer{kConfig};
  auto result = run(normalizer, {quote("2024-03-11T13:30:00Z", "10.00", 1, "10.00", 1)});
  EXPECT_EQ(result.at("states").at("AAPL").at("quote_state"), "locked");
  EXPECT_EQ(result.at("states").at("AAPL").at("spread_micro_usd"), 0);
  EXPECT_EQ(result.at("states").at("AAPL").at("freshness").at("spread_quality"),
            "locked_or_crossed");
  result = run(normalizer, {quote("2024-03-11T13:30:01Z", "10.02", 1, "10.00", 1)});
  EXPECT_EQ(result.at("states").at("AAPL").at("quote_state"), "crossed");
  EXPECT_EQ(result.at("states").at("AAPL").at("spread_micro_usd"), -20'000);
  EXPECT_TRUE(result.at("states").at("AAPL").at("midpoint_micro_usd").is_null());
}

TEST(LiveNormalizerTest, AStaleQuoteMarksItsSpreadAndFreshnessUsesMonotonicReceiptAge) {
  live::LiveNormalizer normalizer{kConfig};
  EXPECT_TRUE(Json::parse(normalizer.snapshot(kWall, 0)).at("states").empty());
  run(normalizer, {quote("2024-03-11T13:30:00Z", "10.00", 1, "10.02", 1)}, 0);
  auto current = state(normalizer, 14 * kSecond);
  EXPECT_EQ(current.at("freshness").at("status"), "fresh");
  EXPECT_EQ(current.at("freshness").at("quote_age_ns"), 14 * kSecond);
  EXPECT_FALSE(current.at("freshness").at("quote_stale").get<bool>());
  current = state(normalizer, 16 * kSecond);
  EXPECT_EQ(current.at("freshness").at("status"), "stale");
  EXPECT_TRUE(current.at("freshness").at("quote_stale").get<bool>());
  EXPECT_EQ(current.at("freshness").at("spread_quality"), "stale_quote");
  // The spread is still reported, marked, rather than silently presented as current.
  EXPECT_EQ(current.at("spread_micro_usd"), 20'000);
  EXPECT_TRUE(current.at("freshness").at("trade_age_ns").is_null());

  // A fresh trade does not refresh the quote: the two ages are disclosed separately.
  run(normalizer, {trade("2024-03-11T13:30:20Z", "10.01", 1)}, 20 * kSecond);
  current = state(normalizer, 21 * kSecond);
  EXPECT_EQ(current.at("freshness").at("status"), "fresh");
  EXPECT_EQ(current.at("freshness").at("trade_age_ns"), kSecond);
  EXPECT_TRUE(current.at("freshness").at("quote_stale").get<bool>());
  EXPECT_FALSE(current.at("freshness").at("trade_stale").get<bool>());
  EXPECT_TRUE(current.at("freshness").at("skew_alert").get<bool>());
  EXPECT_EQ(current.at("quote_trade_skew_ns"), 20 * kSecond);
}

TEST(LiveNormalizerTest, OutOfOrderAndDuplicateRecordsAreFlaggedAndDoNotReplaceState) {
  live::LiveNormalizer normalizer{kConfig};
  const auto newest = trade("2024-03-11T13:30:05Z", "10.05", 1, 5);
  run(normalizer, {newest});
  const auto result = run(normalizer, {trade("2024-03-11T13:30:04Z", "10.04", 1, 4), newest,
                                       quote("2024-03-11T13:30:01Z", "10.00", 1, "10.02", 1)});
  ASSERT_EQ(result.at("events").size(), 3U);
  EXPECT_EQ(result.at("events").at(0).at("classification"), "out_of_order");
  EXPECT_FALSE(result.at("events").at(0).at("applied").get<bool>());
  EXPECT_EQ(result.at("events").at(1).at("classification"), "duplicate");
  // Trades and quotes are ordered independently: an older quote is not late.
  EXPECT_EQ(result.at("events").at(2).at("classification"), "in_order");
  const auto& snapshot = result.at("states").at("AAPL");
  EXPECT_EQ(snapshot.at("last_trade").at("price_micro_usd"), 10'050'000);
  EXPECT_EQ(snapshot.at("counts").at("out_of_order"), 1);
  EXPECT_EQ(snapshot.at("counts").at("duplicates"), 1);
  EXPECT_EQ(snapshot.at("counts").at("trades"), 3);
  // Equal source timestamps are not out of order; the later arrival is kept.
  const auto equal = run(normalizer, {trade("2024-03-11T13:30:05Z", "10.06", 1, 6)});
  EXPECT_EQ(equal.at("events").at(0).at("classification"), "in_order");
  EXPECT_EQ(equal.at("states").at("AAPL").at("last_trade").at("price_micro_usd"), 10'060'000);
}

TEST(LiveNormalizerTest, DuplicateDetectionIsBoundedByItsWindow) {
  live::LiveNormalizer normalizer{R"({"provider":"alpaca","feed":"test","duplicate_window":2})"};
  const auto first = trade("2024-03-11T13:30:00Z", "1", 1, 1);
  run(normalizer,
      {first, trade("2024-03-11T13:30:01Z", "1", 1, 2), trade("2024-03-11T13:30:02Z", "1", 1, 3)});
  // The first record has left the window: it is late, but no longer known as a repeat.
  EXPECT_EQ(run(normalizer, {first}).at("events").at(0).at("classification"), "out_of_order");
}

TEST(LiveNormalizerTest, ProcessingIsDeterministicAndIndependentOfBatchBoundaries) {
  const std::vector<Json> messages = {
      quote("2024-03-11T13:30:00.000000001Z", "10.00", 1, "10.02", 2),
      trade("2024-03-11T13:30:00.5Z", "10.01", 3, 1),
      trade("2024-03-11T13:30:00.4Z", "10.00", 1, 2),
      quote("2024-03-11T13:30:01Z", "10.01", 1, "0", 0),
      trade("2024-03-11T13:30:00.5Z", "10.01", 3, 1),
      Json{{"T", "c"}, {"S", "AAPL"}},
      trade("2024-03-11T13:30:02Z", "bad", 1, 3),
  };
  live::LiveNormalizer whole{kConfig};
  live::LiveNormalizer again{kConfig};
  const auto first = whole.process_batch(batch(messages).dump());
  EXPECT_EQ(first, again.process_batch(batch(messages).dump()));
  EXPECT_EQ(Json::parse(first).at("unsupported").at("c"), 1);
  EXPECT_EQ(Json::parse(first).at("rejects").size(), 1U);

  live::LiveNormalizer split{kConfig};
  Json events = Json::array();
  for (const auto& message : messages) {
    const auto part = run(split, {message});
    for (const auto& event : part.at("events")) {
      events.push_back(event);
    }
  }
  EXPECT_EQ(Json::parse(first).at("events"), events);
  EXPECT_EQ(Json::parse(whole.snapshot(kWall, 0)).at("states"),
            Json::parse(split.snapshot(kWall, 0)).at("states"));
  EXPECT_GE(whole.last_batch_elapsed_ns(), 0);
}

TEST(LiveNormalizerTest, AMalformedBatchIsRefusedWithoutChangingState) {
  live::LiveNormalizer normalizer{kConfig};
  run(normalizer, {trade("2024-03-11T13:30:00Z", "1", 1)});
  const auto before = normalizer.snapshot(kWall, 0);
  EXPECT_THROW(normalizer.process_batch("not json"), RunError);
  EXPECT_THROW(normalizer.process_batch(R"({"frames":[]})"), RunError);
  auto broken = batch({trade("2024-03-11T13:30:01Z", "2", 1, 2)});
  broken["frames"].push_back(Json{{"frame", 2}});
  EXPECT_THROW(normalizer.process_batch(broken.dump()), RunError);
  EXPECT_EQ(normalizer.snapshot(kWall, 0), before);
  EXPECT_THROW(live::LiveNormalizer{R"({"provider":"alpaca"})"}, RunError);
  EXPECT_THROW(live::LiveNormalizer{R"({"provider":"a","feed":"b","max_symbols":0})"}, RunError);
}

TEST(LiveNormalizerTest, InstrumentCapacityIsBoundedAndSymbolsCanBeReleased) {
  live::LiveNormalizer normalizer{R"({"provider":"alpaca","feed":"iex","max_symbols":1})"};
  auto other = trade("2024-03-11T13:30:00Z", "1", 1);
  other["S"] = "MSFT";
  const auto result = run(normalizer, {trade("2024-03-11T13:30:00Z", "1", 1), other});
  EXPECT_EQ(result.at("events").size(), 1U);
  EXPECT_EQ(result.at("rejects").at(0).at("reason"), "instrument capacity exceeded");
  normalizer.retain_symbols(R"(["MSFT"])");
  EXPECT_EQ(run(normalizer, {other}).at("events").size(), 1U);
  EXPECT_FALSE(Json::parse(normalizer.snapshot(kWall, 0)).at("states").contains("AAPL"));
}

} // namespace
} // namespace markettwin
