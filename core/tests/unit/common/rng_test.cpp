#include "markettwin/common/rng.hpp"
#include "markettwin/common/run_error.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <set>

namespace markettwin {
namespace {

TEST(RngTest, MatchesPublishedSplitMixAndXoshiroVectors) {
  std::uint64_t state = 1'234'567;
  EXPECT_EQ(splitmix64(state), 6457827717110365317ULL);
  EXPECT_EQ(splitmix64(state), 3203168211198807973ULL);
  EXPECT_EQ(splitmix64(state), 9817491932198370423ULL);
  Rng rng{Rng::State{1, 2, 3, 4}};
  EXPECT_EQ(rng.next(), 11520ULL);
  EXPECT_EQ(rng.next(), 0ULL);
  EXPECT_EQ(rng.next(), 1509978240ULL);
  EXPECT_EQ(rng.next(), 1215971899390074240ULL);
  EXPECT_THROW(Rng{Rng::State{}}, RunError);
}

TEST(RngTest, SameSeedRepeatsAndStreamsSeedsAndIdsAreAllDistinct) {
  Rng first{42, RngStream::AgentPolicy, 3};
  Rng second{42, RngStream::AgentPolicy, 3};
  for (int draw = 0; draw < 100; ++draw) {
    EXPECT_EQ(first.next(), second.next());
  }
  EXPECT_EQ(first.state(), second.state());
  std::set<std::uint64_t> firsts;
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  for (const auto seed : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{42}, maximum}) {
    for (const auto stream : {
             RngStream::Fundamental,
             RngStream::AgentPolicy,
             RngStream::AgentWakeup,
             RngStream::AgentLatency,
             RngStream::AgentSignal,
         }) {
      for (const auto id : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{2}, maximum}) {
        firsts.insert(Rng{seed, stream, id}.next());
      }
    }
  }
  EXPECT_EQ(firsts.size(), 4U * 5U * 4U);
}

TEST(RngTest, DrawingFromOneStreamNeverMovesAnother) {
  Rng untouched{9, RngStream::AgentPolicy, 2};
  Rng busy{9, RngStream::AgentPolicy, 1};
  Rng reference{9, RngStream::AgentPolicy, 2};
  for (int draw = 0; draw < 1'000; ++draw) {
    static_cast<void>(busy.next());
  }
  EXPECT_EQ(untouched.next(), reference.next());
}

TEST(RngTest, BoundedSamplersStayInRangeAndCoverIt) {
  Rng rng{1, RngStream::AgentPolicy};
  std::array<int, 7> counts{};
  for (int draw = 0; draw < 70'000; ++draw) {
    const auto value = rng.uniform_below(7);
    ASSERT_LT(value, 7U);
    ++counts.at(value);
  }
  for (const auto count : counts) {
    EXPECT_NEAR(count, 10'000, 500);
  }
  for (int draw = 0; draw < 1'000; ++draw) {
    const auto value = rng.uniform_between(-3, 3);
    EXPECT_GE(value, -3);
    EXPECT_LE(value, 3);
  }
  EXPECT_EQ(rng.uniform_between(5, 5), 5);
  static_cast<void>(rng.uniform_between(std::numeric_limits<std::int64_t>::min(),
                                        std::numeric_limits<std::int64_t>::max()));
  EXPECT_EQ(rng.uniform_below(1), 0U);
  EXPECT_THROW(static_cast<void>(rng.uniform_below(0)), RunError);
  EXPECT_THROW(static_cast<void>(rng.uniform_between(1, 0)), RunError);
  EXPECT_THROW(static_cast<void>(rng.bernoulli_ppm(1'000'001)), RunError);
}

TEST(RngTest, BernoulliHonorsItsProbabilityAndAlwaysConsumesADraw) {
  Rng rng{2, RngStream::AgentPolicy};
  int hits = 0;
  for (int draw = 0; draw < 100'000; ++draw) {
    hits += rng.bernoulli_ppm(250'000) ? 1 : 0;
    EXPECT_FALSE(rng.bernoulli_ppm(0));
    EXPECT_TRUE(rng.bernoulli_ppm(1'000'000));
  }
  EXPECT_NEAR(hits, 25'000, 600);
  Rng certain{3, RngStream::AgentPolicy};
  Rng impossible{3, RngStream::AgentPolicy};
  static_cast<void>(certain.bernoulli_ppm(1'000'000));
  static_cast<void>(impossible.bernoulli_ppm(0));
  EXPECT_EQ(certain.state(), impossible.state());
}

TEST(RngTest, ExponentialHasItsMeanAndVarianceWithoutFloatingPoint) {
  Rng rng{4, RngStream::AgentWakeup};
  constexpr std::uint64_t kMean = 1'000'000;
  constexpr int kDraws = 200'000;
  std::uint64_t sum = 0;
  std::uint64_t below_mean = 0;
  std::uint64_t largest = 0;
  for (int draw = 0; draw < kDraws; ++draw) {
    const auto value = rng.exponential(kMean);
    sum += value;
    below_mean += value < kMean ? 1U : 0U;
    largest = std::max(largest, value);
  }
  EXPECT_NEAR(static_cast<double>(sum) / kDraws, 1e6, 1e4);
  EXPECT_NEAR(static_cast<double>(below_mean) / kDraws, 0.6321, 0.005); // 1 - 1/e
  EXPECT_LT(largest, 37 * kMean);
  EXPECT_GT(largest, 8 * kMean);
  EXPECT_EQ(rng.exponential(0), 0U);
  EXPECT_THROW(static_cast<void>(rng.exponential(1ULL << 56U)), RunError);
  static_cast<void>(rng.exponential((1ULL << 56U) - 1));
}

TEST(RngTest, NormalIsCenteredSymmetricAndUnitVarianceInQ16) {
  Rng rng{5, RngStream::Fundamental};
  constexpr int kDraws = 200'000;
  double sum = 0;
  double squares = 0;
  int within_one = 0;
  for (int draw = 0; draw < kDraws; ++draw) {
    const auto value = static_cast<double>(rng.normal_q16()) / 65'536.0;
    ASSERT_LE(std::abs(value), 6.0);
    sum += value;
    squares += value * value;
    within_one += std::abs(value) < 1.0 ? 1 : 0;
  }
  EXPECT_NEAR(sum / kDraws, 0.0, 0.01);
  EXPECT_NEAR(squares / kDraws, 1.0, 0.02);
  EXPECT_NEAR(static_cast<double>(within_one) / kDraws, 0.6827, 0.01);
}

} // namespace
} // namespace markettwin
