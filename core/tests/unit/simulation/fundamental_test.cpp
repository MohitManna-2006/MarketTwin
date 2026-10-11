#include "markettwin/simulation/fundamental.hpp"

#include <cmath>
#include <gtest/gtest.h>
#include <vector>

namespace markettwin {
namespace {

constexpr FundamentalConfig kConfig{
    .initial_milliticks = 15'000'000,
    .update_interval = SimTime{1'000},
    .volatility_milliticks = 1'000,
    .mean_reversion_ppm = 0,
};

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a seed, then how far to step it.
std::vector<std::int64_t> path(const FundamentalConfig& config, std::uint64_t seed, int steps) {
  FundamentalProcess process{config, seed};
  std::vector<std::int64_t> values;
  for (int step = 0; step < steps; ++step) {
    process.step();
    values.push_back(process.value());
  }
  return values;
}

TEST(FundamentalProcessTest, IsAPureFunctionOfConfigAndSeed) {
  EXPECT_EQ(path(kConfig, 42, 500), path(kConfig, 42, 500));
  EXPECT_NE(path(kConfig, 42, 500), path(kConfig, 43, 500));
  EXPECT_NE(path(kConfig, 0, 50), path(kConfig, 1, 50));
}

TEST(FundamentalProcessTest, StepsHaveTheConfiguredVolatilityAndNoDrift) {
  const auto values = path(kConfig, 3, 100'000);
  double sum = 0;
  double squares = 0;
  auto previous = kConfig.initial_milliticks;
  for (const auto value : values) {
    const auto step = static_cast<double>(value - previous);
    sum += step;
    squares += step * step;
    previous = value;
  }
  const auto count = static_cast<double>(values.size());
  EXPECT_NEAR(sum / count, 0.0, 15.0);
  EXPECT_NEAR(std::sqrt(squares / count), 1'000.0, 20.0);
}

TEST(FundamentalProcessTest, MeanReversionPullsTowardTheAnchorAndZeroVolatilityIsStill) {
  auto config = kConfig;
  config.volatility_milliticks = 0;
  config.mean_reversion_ppm = 100'000;
  FundamentalProcess process{config, 1};
  process.step();
  EXPECT_EQ(process.value(), config.initial_milliticks);
  process.jump(-1'000'000);
  EXPECT_EQ(process.anchor(), 14'000'000); // a jump is permanent, not something to revert
  EXPECT_EQ(process.value(), 14'000'000);
  process.step();
  EXPECT_EQ(process.value(), 14'000'000);

  auto reverting = kConfig;
  reverting.mean_reversion_ppm = 50'000;
  double far = 0;
  for (const auto value : path(reverting, 5, 20'000)) {
    far = std::max(far, std::abs(static_cast<double>(value - kConfig.initial_milliticks)));
  }
  EXPECT_LT(far, 20'000.0); // stationary deviation is about 3200; a free walk reaches ~140000
}

TEST(FundamentalProcessTest, VolatilityRegimeScalesStepsAndValueStaysInBounds) {
  FundamentalProcess process{kConfig, 8};
  EXPECT_EQ(process.volatility_milliticks(), 1'000U);
  process.set_volatility_ppm(4'000'000);
  EXPECT_EQ(process.volatility_milliticks(), 4'000U);
  process.set_volatility_ppm(0);
  process.step();
  EXPECT_EQ(process.value(), kConfig.initial_milliticks);
  EXPECT_EQ(process.updates(), 1U);

  process.jump(-1'000'000'000'000);
  EXPECT_EQ(process.value(), FundamentalProcess::kMinimum);
  process.jump(1'000'000'000'000);
  process.jump(1'000'000'000'000);
  process.jump(1'000'000'000'000);
  process.jump(1'000'000'000'000);
  process.jump(1'000'000'000'000);
  EXPECT_EQ(process.value(), FundamentalProcess::kMaximum);
  EXPECT_EQ(process.anchor(), FundamentalProcess::kMaximum);
}

} // namespace
} // namespace markettwin
