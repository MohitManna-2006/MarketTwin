#include "markettwin/calibration/calibration.hpp"
#include "markettwin/common/run_error.hpp"
#include "markettwin/historical/historical.hpp"

#include "../../test_support.hpp"

#include <filesystem>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace markettwin::calibration {
namespace {
using Json = nlohmann::json;
constexpr std::int64_t kSecond = 1'000'000'000;
constexpr std::int64_t kDollar = 1'000'000;

std::optional<std::int64_t> fact(const Facts& facts, Fact which) {
  return facts.scored.at(static_cast<std::size_t>(which));
}

// Three minutes: a 2-cent market, a 1-cent market, then a one-sided half minute, with
// five trades in the first two minutes.
MarketTape hand_built() {
  TapeBuilder builder{0, 180 * kSecond};
  builder.quote(0, 100 * kDollar, 100, (100 * kDollar) + 20'000, 100);
  builder.trade(10 * kSecond, (100 * kDollar) + 10'000, 5);
  builder.trade(20 * kSecond, 100 * kDollar, 5);
  builder.trade(30 * kSecond, (100 * kDollar) + 10'000, 5);
  builder.quote(60 * kSecond, 100 * kDollar, 100, (100 * kDollar) + 10'000, 100);
  builder.trade(70 * kSecond, (100 * kDollar) + 20'000, 5);
  builder.trade(75 * kSecond, (100 * kDollar) + 10'000, 5);
  builder.quote(90 * kSecond, 100 * kDollar, 100, (100 * kDollar) + 10'000, 100); // repeat
  builder.quote(150 * kSecond, 100 * kDollar, 100, 0, 0);
  return std::move(builder).finish();
}

TEST(CalibrationTest, HandBuiltTapeHasExactFacts) {
  const auto tape = hand_built();
  ASSERT_EQ(tape.records.size(), 8U); // the repeated quote is not an L1 change
  const auto facts = compute_facts(tape);
  EXPECT_EQ(facts.trades, 5U);
  EXPECT_EQ(facts.quotes, 3U);
  EXPECT_EQ(facts.measured_ns, 180U * kSecond);
  EXPECT_EQ(facts.two_sided_ns, 150U * kSecond);
  // 90 s at 1 cent and 60 s at 2 cents.
  EXPECT_EQ(fact(facts, Fact::SpreadCentsP10), 1000);
  EXPECT_EQ(fact(facts, Fact::SpreadCentsP50), 1000);
  EXPECT_EQ(fact(facts, Fact::SpreadCentsP90), 2000);
  EXPECT_EQ(fact(facts, Fact::RelativeSpreadBpsP50), 1000);
  EXPECT_EQ(fact(facts, Fact::TradesPerMinute), 1667);
  EXPECT_EQ(fact(facts, Fact::InterarrivalCv), 985'096); // gaps 10, 10, 40, 5 s
  EXPECT_EQ(fact(facts, Fact::EmptyMinuteShare), 333'333);
  EXPECT_EQ(fact(facts, Fact::TradeReturnLag1Autocorr), -250'000);
  // Boundary midpoints 100.01, 100.005, 100.005, then one-sided: returns -49995 and 0 ppb.
  EXPECT_EQ(fact(facts, Fact::MidpointVol1mBps), 354);
  EXPECT_EQ(fact(facts, Fact::TwoSidedShare), 833'333);
  EXPECT_EQ(fact(facts, Fact::L1UpdatesPerMinute), 1000);
  EXPECT_EQ(fact(facts, Fact::L1MedianDurationMs), 75'000'000); // gaps 60 s and 90 s
  EXPECT_EQ(facts.opening_midpoint, (100 * kDollar) + 10'000);
}

TEST(CalibrationTest, EmptyTapeLeavesFactsUndefinedRatherThanZero) {
  const auto facts = compute_facts(TapeBuilder{0, 60 * kSecond}.finish());
  EXPECT_EQ(fact(facts, Fact::TradesPerMinute), 0);
  EXPECT_EQ(fact(facts, Fact::L1UpdatesPerMinute), 0);
  EXPECT_EQ(fact(facts, Fact::EmptyMinuteShare), 1'000'000);
  for (const auto which : {
           Fact::SpreadCentsP50,
           Fact::InterarrivalCv,
           Fact::TradeReturnLag1Autocorr,
           Fact::MidpointVol1mBps,
           Fact::TwoSidedShare,
           Fact::L1MedianDurationMs,
       }) {
    EXPECT_FALSE(fact(facts, which).has_value());
  }
}

TEST(CalibrationTest, TapeBuilderRejectsDisorderAndRecordsOutsideTheWindow) {
  EXPECT_THROW(TapeBuilder(5, 5), RunError);
  TapeBuilder builder{0, 10};
  builder.trade(5, 1, 1);
  EXPECT_THROW(builder.trade(4, 1, 1), RunError);
  EXPECT_THROW(builder.trade(10, 1, 1), RunError);
  EXPECT_THROW(builder.trade(6, 0, 1), RunError);
  EXPECT_THROW(builder.quote(6, -1, 1, 2, 1), RunError);
}

TEST(CalibrationTest, LockedAndCrossedTimeIsReportedButNotTwoSided) {
  TapeBuilder builder{0, 30 * kSecond};
  builder.quote(0, 100, 1, 100, 1);
  builder.quote(10 * kSecond, 101, 1, 100, 1);
  builder.quote(20 * kSecond, 99, 1, 100, 1);
  const auto facts = compute_facts(std::move(builder).finish());
  EXPECT_EQ(facts.locked_ns, 10U * kSecond);
  EXPECT_EQ(facts.crossed_ns, 10U * kSecond);
  EXPECT_EQ(facts.two_sided_ns, 10U * kSecond);
  EXPECT_EQ(fact(facts, Fact::TwoSidedShare), 333'333);
}

TEST(CalibrationTest, SyntheticProjectionObservesWithoutChangingTheRun) {
  auto config = load_config(std::filesystem::path{MARKETTWIN_SOURCE_DIR} / "configs/baseline.json");
  config.limits.horizon = SimTime{600U * kSecond};
  RunSummary observed;
  const auto tape = synthetic_tape(config, &observed);
  Simulation plain{config};
  const auto summary = plain.run();
  EXPECT_EQ(observed.trace_digest, summary.trace_digest);
  EXPECT_EQ(observed.book_digest, summary.book_digest);
  EXPECT_EQ(observed.metrics_digest, summary.metrics_digest);

  const auto facts = compute_facts(tape);
  EXPECT_EQ(facts.trades, summary.trade_count);
  EXPECT_EQ(facts.window_ns, 600U * kSecond);
  for (const auto& value : facts.scored) {
    EXPECT_TRUE(value.has_value());
  }
  // Baseline makers quote 2 ticks either side of their reference; ticks are cents.
  EXPECT_GE(fact(facts, Fact::SpreadCentsP50).value_or(0), 1000);
  EXPECT_EQ(compute_facts(synthetic_tape(config)), facts);

  const auto document = Json::parse(synthetic_facts_json(canonical_config(config)));
  EXPECT_EQ(document.at("run").at("config_digest"), summary.config_digest);
  EXPECT_EQ(parse_facts(document.dump()), facts);
}

TEST(CalibrationTest, ObservedProjectionReadsOnlyTheWindowOfAnImportedSession) {
  const test::TempDirectory root;
  const auto raw = root.path() / "raw.jsonl";
  test::write(
      raw,
      R"({"kind":"quote","t":"2026-10-19T14:29:59Z","bp":"250.00","ap":"250.02","bs":100,"as":100}
{"kind":"quote","t":"2026-10-19T14:30:00Z","bp":"250.00","ap":"250.02","bs":100,"as":100}
{"kind":"trade","t":"2026-10-19T14:30:10Z","p":"250.01","s":10}
{"kind":"quote","t":"2026-10-19T14:30:20Z","bp":"250.00","ap":"250.02","bs":100,"as":100}
{"kind":"quote","t":"2026-10-19T14:30:30Z","bp":"250.00","ap":"250.01","bs":100,"as":100}
{"kind":"trade","t":"2026-10-19T14:31:00Z","p":"250.01","s":10}
)");
  const auto session = root.path() / "session";
  static_cast<void>(historical::import_jsonl(
      raw, session,
      R"({"symbol":"AAPL","provider":"fixture","feed":"fabricated_l1","acquired_at":"2026-10-20T00:00:00Z"})"));
  constexpr std::int64_t kStart = 1'792'420'200LL * kSecond; // 2026-10-19T14:30:00Z
  const auto tape = observed_tape(session, kStart, kStart + (60 * kSecond));
  // The quote before the window and the trade at its end are outside; the 14:30:20 quote
  // repeats the L1 state and is dropped.
  ASSERT_EQ(tape.records.size(), 3U);
  EXPECT_EQ(tape.records.at(0).time_ns, kStart);
  EXPECT_EQ(tape.records.at(1).kind, TapeRecord::Kind::Trade);
  EXPECT_EQ(tape.records.at(1).price, 250'010'000);
  EXPECT_EQ(tape.records.at(2).ask, 250'010'000);
  const auto facts = compute_facts(tape);
  EXPECT_EQ(fact(facts, Fact::SpreadCentsP50), 1000);
  EXPECT_EQ(fact(facts, Fact::SpreadCentsP10), 1000);
  EXPECT_EQ(fact(facts, Fact::SpreadCentsP90), 2000);
  EXPECT_EQ(Json::parse(observed_facts_json(session, kStart, kStart + (60 * kSecond))),
            Json::parse(facts_json(facts)));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): one value, then a trade count.
Facts with_values(std::int64_t value, std::uint64_t trades = 100) {
  Facts facts;
  facts.trades = trades;
  for (auto& item : facts.scored) {
    item = value;
  }
  facts.scored.at(static_cast<std::size_t>(Fact::TwoSidedShare)) = 900'000;
  facts.opening_midpoint = (250 * kDollar) + ((value % 7) * 5'000);
  return facts;
}

TEST(CalibrationTest, TargetsFloorTheScaleAndScoresWeighGroupsEqually) {
  const std::vector<Facts> train{with_values(10'000), with_values(10'000), with_values(10'000)};
  const auto target = targets(train);
  EXPECT_EQ(target.sessions, 3U);
  const auto spread = static_cast<std::size_t>(Fact::SpreadCentsP50);
  EXPECT_EQ(target.mean.at(spread), 10'000);
  EXPECT_EQ(target.deviation.at(spread), 0);
  // Identical sessions: 5% of 10000 is 500, above the 100 floor.
  EXPECT_EQ(target.scale.at(spread), 500);
  const auto two_sided = static_cast<std::size_t>(Fact::TwoSidedShare);
  EXPECT_EQ(target.scale.at(two_sided), 45'000);
  EXPECT_EQ(target.scale.at(static_cast<std::size_t>(Fact::TradesPerMinute)), 1000);
  EXPECT_EQ(target.reference_price_cents, 25'002); // every opening midpoint is 250.02

  const std::vector<Facts> exact{with_values(10'000), with_values(10'000)};
  const auto perfect = score(target, target, exact);
  ASSERT_TRUE(perfect.valid);
  EXPECT_EQ(perfect.score_ppm, 0U);

  // One spread fact off by two scales: (2e6)^2 * (12 / 4) / 48, square-rooted.
  auto shifted = exact;
  for (auto& seed : shifted) {
    seed.scored.at(spread) = 11'000;
  }
  const auto off = score(target, target, shifted);
  ASSERT_TRUE(off.valid);
  EXPECT_EQ(off.facts.at(spread).distance_ppm, 2'000'000);
  EXPECT_EQ(off.score_ppm, 500'000U);

  const std::vector<Facts> thin{with_values(10'000, 29)};
  EXPECT_FALSE(score(target, target, thin).valid);
  auto undefined = exact;
  undefined.at(1).scored.at(spread).reset();
  const auto missing = score(target, target, undefined);
  EXPECT_FALSE(missing.valid);
  EXPECT_FALSE(missing.facts.at(spread).mean.has_value());
}

TEST(CalibrationTest, JsonBoundariesRoundTrip) {
  const auto facts = compute_facts(hand_built());
  EXPECT_EQ(parse_facts(facts_json(facts)), facts);
  const std::vector<Facts> train{with_values(10'000), with_values(12'000)};
  const auto target = targets(train);
  EXPECT_EQ(parse_targets(targets_json(target)), target);
  const Json array = {
      Json::parse(facts_json(with_values(10'000))),
      Json::parse(facts_json(with_values(12'000))),
  };
  EXPECT_EQ(Json::parse(targets_from_facts_json(array.dump())), Json::parse(targets_json(target)));
  const auto scored =
      Json::parse(score_from_json(targets_json(target), targets_json(target), array.dump()));
  EXPECT_EQ(scored.at("valid"), true);
  EXPECT_EQ(scored.at("seeds"), 2);
  EXPECT_THROW(static_cast<void>(parse_facts("{}")), RunError);
  EXPECT_THROW(static_cast<void>(targets_from_facts_json("{}")), RunError);
}

} // namespace
} // namespace markettwin::calibration
