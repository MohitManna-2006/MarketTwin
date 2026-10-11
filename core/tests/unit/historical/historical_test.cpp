#include "markettwin/common/run_error.hpp"
#include "markettwin/historical/historical.hpp"

#include "../../test_support.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>

namespace markettwin {
namespace {
using Json = nlohmann::json;
constexpr auto kMetadata =
    R"({"symbol":"AAPL","provider":"fixture","feed":"fabricated_l1","acquired_at":"2024-03-11T00:00:00Z"})";

void file(const std::filesystem::path& path, const std::string& contents) {
  std::ofstream output{path};
  output << contents;
}

TEST(HistoricalTest, ImportPreservesPrecisionAndReplaysWithoutSyntheticExchange) {
  const test::TempDirectory root;
  const auto input = std::filesystem::path{MARKETTWIN_SOURCE_DIR} /
                     "core/tests/fixtures/historical/aapl_records.jsonl";
  const auto first = Json::parse(historical::import_jsonl(input, root.path() / "first", kMetadata));
  const auto second =
      Json::parse(historical::import_jsonl(input, root.path() / "second", kMetadata));
  EXPECT_EQ(first.at("canonical_sha256"), second.at("canonical_sha256"));
  EXPECT_EQ(first.at("record_count"), 5);
  EXPECT_EQ(Json::parse(historical::verify(root.path() / "first")).at("verified"), true);
  std::ifstream events{root.path() / "first/historical_events.jsonl"};
  std::string line;
  ASSERT_TRUE(static_cast<bool>(std::getline(events, line)));
  EXPECT_EQ(Json::parse(line).at("bid_micro_usd"), 170123456);
  EXPECT_EQ(Json::parse(line).at("timestamp_precision_digits"), 9);
  const Json quality = Json::parse(test::read(root.path() / "first/quality_report.json"));
  EXPECT_EQ(quality.at("locked_quotes"), 1);
  EXPECT_EQ(quality.at("crossed_quotes"), 1);
  EXPECT_EQ(quality.at("traded_quantity"), 65);
  const Json metrics = Json::parse(test::read(root.path() / "first/historical_metrics.json"));
  EXPECT_EQ(metrics.at("trade_count"), 2);
  EXPECT_EQ(metrics.at("vwap_micro_usd"), 170128077);
  EXPECT_EQ(metrics.at("quote_state_time").at("locked_ns"), 1);
  EXPECT_EQ(metrics.at("quote_state_time").at("crossed_ns"), 1);
}

TEST(HistoricalTest, RejectsPrecisionLossAndInvalidQuantity) {
  const test::TempDirectory root;
  const auto input = root.path() / "raw.jsonl";
  file(input, R"({"kind":"trade","t":"2024-03-11T13:30:00Z","p":"1.0000001","s":1})"
              "\n");
  EXPECT_THROW(historical::import_jsonl(input, root.path() / "too_precise", kMetadata), RunError);
  file(input, R"({"kind":"trade","t":"2024-03-11T13:30:00Z","p":"1.00","s":0})"
              "\n");
  EXPECT_THROW(historical::import_jsonl(input, root.path() / "zero_size", kMetadata), RunError);
}

TEST(HistoricalTest, SortsEqualTimestampsByProviderSequenceAndDetectsDuplicate) {
  const test::TempDirectory root;
  const auto input = root.path() / "raw.jsonl";
  const auto* const late =
      R"({"kind":"trade","t":"2024-03-11T13:30:00Z","p":"2","s":1,"sequence":2})";
  const auto* const early =
      R"({"kind":"trade","t":"2024-03-11T13:30:00Z","p":"1","s":1,"sequence":1})";
  file(input, std::string{late} + "\n" + early + "\n" + early + "\n");
  historical::import_jsonl(input, root.path() / "ordered", kMetadata);
  std::ifstream events{root.path() / "ordered/historical_events.jsonl"};
  std::string line;
  ASSERT_TRUE(static_cast<bool>(std::getline(events, line)));
  EXPECT_EQ(Json::parse(line).at("price_micro_usd"), 1000000);
  EXPECT_EQ(Json::parse(test::read(root.path() / "ordered/quality_report.json")).at("duplicates"),
            1);
}

TEST(HistoricalTest, TamperedSampleFailsVerification) {
  const test::TempDirectory root;
  const auto input = std::filesystem::path{MARKETTWIN_SOURCE_DIR} /
                     "core/tests/fixtures/historical/aapl_records.jsonl";
  historical::import_jsonl(input, root.path() / "session", kMetadata);
  file(root.path() / "session/historical_samples.jsonl", "tampered\n");
  const auto result = Json::parse(historical::verify(root.path() / "session"));
  EXPECT_FALSE(result.at("verified").get<bool>());
  EXPECT_EQ(result.at("code"), "DigestMismatch");
}

TEST(HistoricalTest, EmptyAndOneSidedSessionsRemainExplicit) {
  const test::TempDirectory root;
  const auto empty = root.path() / "empty.jsonl";
  file(empty, "");
  historical::import_jsonl(empty, root.path() / "empty", kMetadata);
  const auto empty_manifest = Json::parse(test::read(root.path() / "empty/manifest.json"));
  EXPECT_EQ(empty_manifest.at("record_count"), 0);
  EXPECT_TRUE(Json::parse(historical::verify(root.path() / "empty")).at("verified"));

  const auto one_sided = root.path() / "one-sided.jsonl";
  file(one_sided,
       R"({"kind":"quote","t":"2024-03-11T13:30:00Z","bp":"1.00","ap":"0","bs":2,"as":0})"
       "\n"
       R"({"kind":"quote","t":"2024-03-11T13:40:01Z","bp":"0","ap":"1.01","bs":0,"as":3})"
       "\n");
  historical::import_jsonl(one_sided, root.path() / "one-sided", kMetadata);
  const Json quality = Json::parse(test::read(root.path() / "one-sided/quality_report.json"));
  EXPECT_EQ(quality.at("gaps_over_five_minutes"), 1);
  EXPECT_EQ(quality.at("gap_markers").at(0).at("kind"), "quote");
  std::ifstream samples{root.path() / "one-sided/historical_samples.jsonl"};
  std::string line;
  ASSERT_TRUE(static_cast<bool>(std::getline(samples, line)));
  EXPECT_EQ(Json::parse(line).at("quote_state"), "bid_only");
  ASSERT_TRUE(static_cast<bool>(std::getline(samples, line)));
  EXPECT_EQ(Json::parse(line).at("quote_state"), "ask_only");
}

TEST(HistoricalTest, RejectsOverflowAndWrongInstrument) {
  const test::TempDirectory root;
  const auto input = root.path() / "raw.jsonl";
  file(input, R"({"kind":"trade","t":"2024-03-11T13:30:00Z","p":"9223372036854775807","s":1})"
              "\n");
  EXPECT_THROW(historical::import_jsonl(input, root.path() / "overflow", kMetadata), RunError);
  file(input, R"({"kind":"trade","symbol":"MSFT","t":"2024-03-11T13:30:00Z","p":"1","s":1})"
              "\n");
  EXPECT_THROW(historical::import_jsonl(input, root.path() / "instrument", kMetadata), RunError);
}

TEST(HistoricalTest, FailedImportDoesNotPublishPartialSession) {
  const test::TempDirectory root;
  const auto input = root.path() / "raw.jsonl";
  constexpr auto kQuantity = "9223372036854775807";
  file(input, std::string{R"({"kind":"trade","t":"2024-03-11T13:30:00Z","p":"1","s":)"} +
                  kQuantity + "}\n" + R"({"kind":"trade","t":"2024-03-11T13:30:01Z","p":"1","s":)" +
                  kQuantity + "}\n" + R"({"kind":"trade","t":"2024-03-11T13:30:02Z","p":"1","s":)" +
                  kQuantity + "}\n");
  const auto output = root.path() / "overflow";
  EXPECT_THROW(historical::import_jsonl(input, output, kMetadata), RunError);
  EXPECT_FALSE(std::filesystem::exists(output));
  EXPECT_FALSE(std::filesystem::exists(root.path() / "overflow.partial"));
}

TEST(HistoricalTest, RejectsAnotherEngineVersion) {
  const test::TempDirectory root;
  const auto input = std::filesystem::path{MARKETTWIN_SOURCE_DIR} /
                     "core/tests/fixtures/historical/aapl_records.jsonl";
  historical::import_jsonl(input, root.path() / "session", kMetadata);
  auto manifest = Json::parse(test::read(root.path() / "session/manifest.json"));
  manifest.at("engine_version") = "incompatible";
  file(root.path() / "session/manifest.json", manifest.dump() + "\n");
  EXPECT_THROW(historical::verify(root.path() / "session"), RunError);
}

TEST(HistoricalTest, RejectsInvalidManifestIdentityAndOptionalFieldTypes) {
  const test::TempDirectory root;
  const auto input = root.path() / "raw.jsonl";
  file(input, R"({"kind":"trade","t":"2024-03-11T13:30:00Z","p":"1","s":1,"c":"bad"})"
              "\n");
  EXPECT_THROW(historical::import_jsonl(input, root.path() / "conditions", kMetadata), RunError);
  file(input, R"({"kind":"trade","t":"2024-03-11T13:30:00Z","p":"1","s":1,"i":[]})"
              "\n");
  EXPECT_THROW(historical::import_jsonl(input, root.path() / "trade-id", kMetadata), RunError);

  file(input, R"({"kind":"trade","t":"2024-03-11T13:30:00Z","p":"1","s":1})"
              "\n");
  historical::import_jsonl(input, root.path() / "session", kMetadata);
  auto manifest = Json::parse(test::read(root.path() / "session/manifest.json"));
  manifest.at("kind") = "synthetic_simulation";
  file(root.path() / "session/manifest.json", manifest.dump() + "\n");
  EXPECT_THROW(historical::verify(root.path() / "session"), RunError);
}

TEST(HistoricalTest, ComputesDocumentedTradeArrivalAndReturnVolatility) {
  const test::TempDirectory root;
  const auto input = root.path() / "returns.jsonl";
  file(input, R"({"kind":"trade","t":"2024-03-11T13:30:00Z","p":"100","s":1})"
              "\n"
              R"({"kind":"trade","t":"2024-03-11T13:30:01Z","p":"101","s":1})"
              "\n"
              R"({"kind":"trade","t":"2024-03-11T13:30:02Z","p":"99","s":1})"
              "\n");
  historical::import_jsonl(input, root.path() / "session", kMetadata);
  const Json metrics = Json::parse(test::read(root.path() / "session/historical_metrics.json"));
  EXPECT_EQ(metrics.at("trade_to_trade_return_observations"), 2);
  EXPECT_FALSE(metrics.at("trade_to_trade_return_volatility_ppb").is_null());
  EXPECT_EQ(metrics.at("trade_arrival_rate_millihertz"), 1500);
}
} // namespace
} // namespace markettwin
