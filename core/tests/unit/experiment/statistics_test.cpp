#include "markettwin/common/run_error.hpp"
#include "markettwin/experiment/statistics.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <vector>

namespace markettwin {
namespace {

Summary summary_of(const std::vector<std::int64_t>& values, const BootstrapConfig& bootstrap = {
                                                                .resamples = 0,
                                                            }) {
  const auto summary = summarize(values, bootstrap);
  if (!summary) {
    throw std::logic_error("expected a summary");
  }
  return *summary;
}

TEST(StatisticsTest, MeanMedianDeviationAndTIntervalMatchHandComputedValues) {
  // 1..5: mean 3, s = sqrt(2.5) = 1.5811, t(0.975, 4) = 2.776445, half width 1.9632.
  const auto odd = summary_of({5, 1, 4, 2, 3});
  EXPECT_EQ(odd.count, 5U);
  EXPECT_EQ(odd.minimum, 1);
  EXPECT_EQ(odd.maximum, 5);
  EXPECT_EQ(odd.mean_milli, 3'000);
  EXPECT_EQ(odd.median_milli, 3'000);
  EXPECT_EQ(odd.standard_deviation_milli, 1'581);
  EXPECT_EQ(odd.mean_t_interval, (Interval{1'037, 4'963}));
  EXPECT_FALSE(odd.mean_bootstrap_interval.has_value());

  // Mean 233.333, median (100 + 300) / 2, s = 402.078, t(0.975, 5) = 2.570582.
  const auto even = summary_of({-250, 100, 400, -50, 900, 300});
  EXPECT_EQ(even.mean_milli, 233'333);
  EXPECT_EQ(even.median_milli, 200'000);
  EXPECT_EQ(even.standard_deviation_milli, 402'078);
  EXPECT_EQ(even.mean_t_interval, (Interval{233'333 - 421'955, 233'333 + 421'955}));
}

TEST(StatisticsTest, RoundingIsHalfAwayFromZeroOnBothSides) {
  EXPECT_EQ(summary_of({1, 1, 2}).mean_milli, 1'333);
  EXPECT_EQ(summary_of({-1, -1, -2}).mean_milli, -1'333);
  EXPECT_EQ(summary_of({1, 2, 2}).mean_milli, 1'667);
  EXPECT_EQ(summary_of({-1, -2, -2}).mean_milli, -1'667);
  EXPECT_EQ(summary_of({0, 1}).median_milli, 500);
  EXPECT_EQ(summary_of({-1, 0}).median_milli, -500);
}

TEST(StatisticsTest, EmptyAndSingleValueSamplesReportOnlyWhatIsDefined) {
  EXPECT_FALSE(summarize(std::vector<std::int64_t>{}).has_value());
  const auto single = summary_of({-7}, {.resamples = 1'000, .seed = 1});
  EXPECT_EQ(single.count, 1U);
  EXPECT_EQ(single.mean_milli, -7'000);
  EXPECT_EQ(single.median_milli, -7'000);
  EXPECT_FALSE(single.standard_deviation_milli.has_value());
  EXPECT_FALSE(single.mean_t_interval.has_value());
  EXPECT_FALSE(single.mean_bootstrap_interval.has_value());
}

TEST(StatisticsTest, IdenticalValuesHaveZeroDeviationAndAPointInterval) {
  const auto flat = summary_of({42, 42, 42, 42}, {.resamples = 500, .seed = 9});
  EXPECT_EQ(flat.standard_deviation_milli, 0);
  EXPECT_EQ(flat.mean_t_interval, (Interval{42'000, 42'000}));
  EXPECT_EQ(flat.mean_bootstrap_interval, (Interval{42'000, 42'000}));
}

TEST(StatisticsTest, StudentQuantilesMatchPublishedTablesAndDecreaseToTheNormalQuantile) {
  EXPECT_EQ(student_t_975_micro(1), 12'706'205);
  EXPECT_EQ(student_t_975_micro(2), 4'302'653);
  EXPECT_EQ(student_t_975_micro(10), 2'228'139);
  EXPECT_EQ(student_t_975_micro(30), 2'042'272);
  // Beyond the table the series takes over. Published values: 2.021075, 2.000298, 1.979930.
  EXPECT_EQ(student_t_975_micro(40), 2'021'075);
  EXPECT_EQ(student_t_975_micro(60), 2'000'298);
  EXPECT_EQ(student_t_975_micro(120), 1'979'930);
  EXPECT_EQ(student_t_975_micro(199), 1'971'957);
  EXPECT_EQ(student_t_975_micro(1'000'000'000), 1'959'964);
  for (std::uint64_t df = 1; df < 400; ++df) {
    EXPECT_GT(student_t_975_micro(df), student_t_975_micro(df + 1)) << df;
  }
  EXPECT_THROW(static_cast<void>(student_t_975_micro(0)), RunError);
}

TEST(StatisticsTest, BootstrapIntervalIsDeterministicSeededAndOrderIndependent) {
  std::vector<std::int64_t> values;
  values.reserve(60);
  for (std::int64_t index = 0; index < 60; ++index) {
    values.push_back(((index * 7'919) % 1'001) - 400);
  }
  const BootstrapConfig config{.resamples = 2'000, .seed = 77};
  const auto first = summary_of(values, config);
  ASSERT_TRUE(first.mean_bootstrap_interval.has_value());
  EXPECT_EQ(summary_of(values, config), first);
  // The same values in another order are the same sample.
  auto reversed = values;
  std::ranges::reverse(reversed);
  EXPECT_EQ(summary_of(reversed, config), first);
  // The interval brackets the mean and is of the same order as the t interval.
  const auto& bootstrap = *first.mean_bootstrap_interval;
  EXPECT_LT(bootstrap.lower_milli, first.mean_milli);
  EXPECT_GT(bootstrap.upper_milli, first.mean_milli);
  ASSERT_TRUE(first.mean_t_interval.has_value());
  const auto t_width = first.mean_t_interval->upper_milli - first.mean_t_interval->lower_milli;
  const auto bootstrap_width = bootstrap.upper_milli - bootstrap.lower_milli;
  EXPECT_GT(bootstrap_width * 10, t_width * 8);
  EXPECT_LT(bootstrap_width * 10, t_width * 12);
  // Another seed or stream resamples differently; the exact statistics do not move.
  const auto reseeded = summary_of(values, {.resamples = 2'000, .seed = 78});
  EXPECT_NE(reseeded.mean_bootstrap_interval, first.mean_bootstrap_interval);
  EXPECT_EQ(reseeded.mean_t_interval, first.mean_t_interval);
  const auto other_stream = summarize(values, config, 1);
  ASSERT_TRUE(other_stream.has_value());
  EXPECT_NE(other_stream->mean_bootstrap_interval, first.mean_bootstrap_interval);
}

TEST(StatisticsTest, PinnedBootstrapIntervalHoldsAcrossToolchains) {
  const auto summary = summary_of({-250, 100, 400, -50, 900, 300, 120, -75, 610, 45},
                                  {.resamples = 1'000, .seed = 2'026});
  EXPECT_EQ(summary.mean_milli, 210'000);
  EXPECT_EQ(summary.mean_bootstrap_interval, (Interval{9'000, 401'500}));
}

TEST(StatisticsTest, ValuesAtTheSupportedBoundAreExactAndBeyondItAreRejected) {
  const auto bound = kMaxStatisticMagnitude;
  const auto extreme = summary_of({bound, -bound, bound, -bound});
  EXPECT_EQ(extreme.mean_milli, 0);
  // s^2 = 4 * bound^2 / 3
  EXPECT_EQ(extreme.standard_deviation_milli, 1'154'700'538'379'252);
  ASSERT_TRUE(extreme.mean_t_interval.has_value());
  EXPECT_EQ(extreme.mean_t_interval->lower_milli, -extreme.mean_t_interval->upper_milli);
  EXPECT_THROW(static_cast<void>(summarize(std::vector<std::int64_t>{bound + 1})), RunError);
  EXPECT_THROW(static_cast<void>(summarize(std::vector<std::int64_t>{-bound - 1, 0})), RunError);
  EXPECT_THROW(static_cast<void>(summarize(std::vector<std::int64_t>(kMaxStatisticValues + 1, 1))),
               RunError);
  EXPECT_THROW(static_cast<void>(summarize(std::vector<std::int64_t>{1, 2},
                                           {.resamples = kMaxBootstrapResamples + 1, .seed = 0})),
               RunError);
  const std::vector<std::int64_t> many(kMaxStatisticValues, bound);
  const auto full = summarize(many);
  ASSERT_TRUE(full.has_value());
  EXPECT_EQ(full->mean_milli, bound * 1'000);
  EXPECT_EQ(full->standard_deviation_milli, 0);
}

} // namespace
} // namespace markettwin
