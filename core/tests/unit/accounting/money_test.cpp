#include "markettwin/accounting/fees.hpp"

#include <gtest/gtest.h>
#include <limits>

namespace markettwin {
namespace {

constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
constexpr auto kMin = std::numeric_limits<std::int64_t>::min();

void expect_overflow(const std::function<void()>& action) {
  try {
    action();
    FAIL() << "expected AccountingOverflow";
  } catch (const RunError& error) {
    EXPECT_EQ(error.code(), "AccountingOverflow");
  }
}

TEST(MoneyTest, NotionalIsPriceTimesQuantityTimesTickSizeExactly) {
  EXPECT_EQ(notional(PriceTicks{15'003}, Quantity{14}, 1), Money{210'042});
  EXPECT_EQ(notional(PriceTicks{15'003}, Quantity{14}, 25), Money{5'251'050});
  EXPECT_EQ(notional(PriceTicks{1}, Quantity{0}, 7), Money{0});
  // The largest representable value is accepted; one more share is not.
  EXPECT_EQ(notional(PriceTicks{kMax}, Quantity{1}, 1), Money{kMax});
  EXPECT_EQ(notional(PriceTicks{1}, Quantity{static_cast<std::uint64_t>(kMax)}, 1), Money{kMax});
}

TEST(MoneyTest, EveryOverflowIsAnExplicitError) {
  expect_overflow([] { static_cast<void>(notional(PriceTicks{kMax}, Quantity{2}, 1)); });
  expect_overflow([] { static_cast<void>(notional(PriceTicks{kMax}, Quantity{1}, 2)); });
  expect_overflow(
      [] { static_cast<void>(notional(PriceTicks{1}, Quantity{~std::uint64_t{0}}, 1)); });
  expect_overflow(
      [] { static_cast<void>(notional(PriceTicks{2'147'483'648}, Quantity{2'147'483'648}, 2)); });
  expect_overflow([] { static_cast<void>(checked_add(Money{kMax}, Money{1})); });
  expect_overflow([] { static_cast<void>(checked_subtract(Money{kMin}, Money{1})); });
  expect_overflow([] { static_cast<void>(checked_negate(Money{kMin})); });
  expect_overflow([] { static_cast<void>(mark_value(kMax, 4, 1)); });
  expect_overflow([] { static_cast<void>(mark_value(2, 2, ~std::uint64_t{0})); });
  EXPECT_EQ(checked_add(Money{kMax - 1}, Money{1}), Money{kMax});
  EXPECT_EQ(checked_negate(Money{kMax}), Money{-kMax});
  EXPECT_EQ(mark_value(kMax, 2, 1), Money{kMax});
  EXPECT_EQ(mark_value(-kMax, 2, 1), Money{-kMax});
}

TEST(MoneyTest, RoundedDivisionSendsTiesAwayFromZero) {
  EXPECT_EQ(divide_round(5, 2), 3);
  EXPECT_EQ(divide_round(-5, 2), -3);
  EXPECT_EQ(divide_round(4, 3), 1);
  EXPECT_EQ(divide_round(-4, 3), -1);
  EXPECT_EQ(divide_round(5, 3), 2);
  EXPECT_EQ(divide_round(-5, 3), -2);
  EXPECT_EQ(divide_round(0, 9), 0);
  EXPECT_EQ(divide_round(6, 3), 2);
  EXPECT_THROW(static_cast<void>(divide_round(1, 0)), RunError);
  EXPECT_THROW(static_cast<void>(divide_round(1, -1)), RunError);
}

TEST(MoneyTest, MarkValueRoundsAHalfMinorUnitAwayFromZeroForLongsAndShorts) {
  // Three shares at a 100.5 tick midpoint are worth 301.5 minor units.
  EXPECT_EQ(mark_value(3, 201, 1), Money{302});
  EXPECT_EQ(mark_value(-3, 201, 1), Money{-302});
  EXPECT_EQ(mark_value(2, 201, 1), Money{201});
  EXPECT_EQ(mark_value(3, 201, 2), Money{603}); // an even tick size is always exact
  EXPECT_EQ(mark_value(0, 201, 1), Money{0});
  EXPECT_EQ(half_tick_shares_to_money(7, 1), Money{4});
  EXPECT_EQ(half_tick_shares_to_money(-7, 1), Money{-4});
  EXPECT_EQ(half_tick_shares_to_money(-7, 10), Money{-35});
}

TEST(MoneyTest, ProratingTruncatesTowardZeroAndReleasesEverythingAtTheEnd) {
  EXPECT_EQ(prorate(Money{1'000}, 1, 3), Money{333});
  EXPECT_EQ(prorate(Money{-1'000}, 1, 3), Money{-333});
  EXPECT_EQ(prorate(Money{1'000}, 3, 3), Money{1'000});
  EXPECT_EQ(prorate(Money{kMax}, 7, 7), Money{kMax});
  EXPECT_EQ(prorate(Money{kMax}, ~std::uint64_t{0} - 1, ~std::uint64_t{0}), Money{kMax - 1});
  EXPECT_THROW(static_cast<void>(prorate(Money{1}, 0, 3)), RunError);
  EXPECT_THROW(static_cast<void>(prorate(Money{1}, 4, 3)), RunError);
}

TEST(MoneyTest, FeesRoundPerFillWithTiesAwayFromZeroAndRebatesAreNegative) {
  EXPECT_EQ(fee_for(Money{1'000}, 2'500), Money{3}); // 2.5
  EXPECT_EQ(fee_for(Money{1'000}, -1'000), Money{-1});
  EXPECT_EQ(fee_for(Money{500}, 1'000), Money{1});   // 0.5
  EXPECT_EQ(fee_for(Money{500}, -1'000), Money{-1}); // -0.5
  EXPECT_EQ(fee_for(Money{499}, 1'000), Money{0});
  EXPECT_EQ(fee_for(Money{123'456}, 0), Money{0});
  EXPECT_EQ(fee_for(Money{kMax}, 1'000'000), Money{kMax});
}

TEST(MoneyTest, IntegerSquareRootIsTheFloorOfTheRoot) {
  EXPECT_EQ(integer_sqrt(0), 0U);
  EXPECT_EQ(integer_sqrt(1), 1U);
  EXPECT_EQ(integer_sqrt(15), 3U);
  EXPECT_EQ(integer_sqrt(16), 4U);
  EXPECT_EQ(integer_sqrt(17), 4U);
  const UInt128 large = (UInt128{1} << 100U) + 12'345;
  const auto root = integer_sqrt(large);
  EXPECT_LE(root * root, large);
  EXPECT_GT((root + 1) * (root + 1), large);
  EXPECT_EQ(integer_sqrt(~UInt128{0}), ~std::uint64_t{0});
}

} // namespace
} // namespace markettwin
