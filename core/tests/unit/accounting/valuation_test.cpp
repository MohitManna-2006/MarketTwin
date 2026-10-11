#include "markettwin/accounting/valuation.hpp"

#include "../../test_support.hpp"

#include <gtest/gtest.h>
#include <limits>

namespace markettwin {
namespace {

Mark mark(std::optional<std::int64_t> bid, std::optional<std::int64_t> ask,
          std::optional<std::int64_t> last, std::optional<std::int64_t> reference) {
  const auto price = [](std::optional<std::int64_t> ticks) {
    return ticks ? std::optional{PriceTicks{*ticks}} : std::nullopt;
  };
  return select_mark(price(bid), price(ask), price(last), price(reference));
}

// Agent 1 is long `shares` at 100 ticks, bought from agent 2.
Ledger position(std::uint64_t shares) {
  const std::vector<AccountOpening> openings{
      {.agent = AgentId{1}, .cash = Money{1'000}, .position = 0, .cost_basis = {}},
      {.agent = AgentId{2}, .cash = Money{0}, .position = 0, .cost_basis = {}},
  };
  Ledger books{openings, FeeSchedule(), 1};
  if (shares != 0) {
    static_cast<void>(books.apply(test::trade(1, 1, 2, 100, shares)));
  }
  return books;
}

TEST(ValuationTest, MarkHierarchyIsMidpointThenLastTradeThenReferenceThenNothing) {
  EXPECT_EQ(mark(100, 103, 90, 80), (Mark{MarkSource::Midpoint, 203}));
  EXPECT_EQ(mark(100, 100, {}, {}), (Mark{MarkSource::Midpoint, 200}));
  // A one-sided book is not a price: fall back to the last trade, on either side.
  EXPECT_EQ(mark(100, {}, 90, 80), (Mark{MarkSource::LastTrade, 180}));
  EXPECT_EQ(mark({}, 103, 90, 80), (Mark{MarkSource::LastTrade, 180}));
  EXPECT_EQ(mark({}, {}, 90, 80), (Mark{MarkSource::LastTrade, 180})); // empty book
  // Nothing has traded: only then is the reference used, with its source recorded.
  EXPECT_EQ(mark(100, {}, {}, 80), (Mark{MarkSource::Reference, 160}));
  EXPECT_EQ(mark({}, {}, {}, 80), (Mark{MarkSource::Reference, 160}));
  EXPECT_EQ(mark(100, {}, {}, {}), Mark{});
  EXPECT_EQ(mark({}, {}, {}, {}).source, MarkSource::None);
  EXPECT_EQ(mark_source_name(MarkSource::LastTrade), "LastTrade");
  EXPECT_EQ(mark_source_name(MarkSource::None), "None");
  EXPECT_THROW(static_cast<void>(mark(std::numeric_limits<std::int64_t>::max(), 1, {}, {})),
               RunError);
}

TEST(ValuationTest, EquityIsCashPlusMarkedInventoryAndPnlSplitsExactly) {
  const auto books = position(10);
  const auto& longs = books.account(AgentId{1});
  const auto& shorts = books.account(AgentId{2});
  const auto up = value_account(longs, mark(104, 106, {}, {}), 1);
  ASSERT_TRUE(up.has_value());
  EXPECT_EQ(up->market_value, Money{1'050});
  EXPECT_EQ(up->unrealized_pnl, Money{50});
  EXPECT_EQ(up->equity, Money{1'050}); // 1000 opening cash - 1000 paid + 1050 of shares
  EXPECT_EQ(up->total_pnl, Money{50});
  const auto down = value_account(shorts, mark(104, 106, {}, {}), 1);
  ASSERT_TRUE(down.has_value());
  EXPECT_EQ(down->market_value, Money{-1'050});
  EXPECT_EQ(down->unrealized_pnl, Money{-50});
  EXPECT_EQ(down->equity, Money{-50});
  EXPECT_EQ(down->total_pnl, Money{-50});
  // total = realized - fees + unrealized, for every account and any mark.
  for (const auto* account : {&longs, &shorts}) {
    const auto valuation = value_account(*account, mark({}, {}, 97, {}), 1);
    ASSERT_TRUE(valuation.has_value());
    EXPECT_EQ(valuation->total_pnl,
              checked_add(checked_subtract(account->realized_pnl, account->fees_paid),
                          valuation->unrealized_pnl));
  }
}

TEST(ValuationTest, AHalfTickMidpointRoundsSymmetricallyForLongAndShort) {
  const auto books = position(3);
  const auto half = mark(100, 101, {}, {}); // midpoint 100.5
  const auto longs = value_account(books.account(AgentId{1}), half, 1);
  const auto shorts = value_account(books.account(AgentId{2}), half, 1);
  ASSERT_TRUE(longs.has_value());
  ASSERT_TRUE(shorts.has_value());
  EXPECT_EQ(longs->market_value, Money{302});   // 301.5 rounds away from zero
  EXPECT_EQ(shorts->market_value, Money{-302}); // and so offsetting positions still net to zero
  EXPECT_EQ(longs->unrealized_pnl, Money{2});
  // With an even tick size the same mark is exact.
  const auto even = value_account(books.account(AgentId{1}), half, 2);
  ASSERT_TRUE(even.has_value());
  EXPECT_EQ(even->market_value, Money{603});
}

TEST(ValuationTest, AMissingMarkIsMissingNotZeroAndAFlatAccountNeedsNone) {
  const auto open = position(10);
  const Mark none;
  EXPECT_FALSE(value_account(open.account(AgentId{1}), none, 1).has_value());
  EXPECT_FALSE(value_account(open.account(AgentId{2}), none, 1).has_value());
  const auto flat = position(0);
  const auto valuation = value_account(flat.account(AgentId{1}), none, 1);
  ASSERT_TRUE(valuation.has_value());
  EXPECT_EQ(valuation->equity, Money{1'000});
  EXPECT_EQ(valuation->unrealized_pnl, Money{0});
  EXPECT_EQ(valuation->total_pnl, Money{0});
}

TEST(ValuationTest, DrawdownIsTheLargestFallFromARunningPeak) {
  EquityTracker tracker;
  EXPECT_EQ(tracker.observations(), 0U);
  EXPECT_FALSE(tracker.peak().has_value());
  EXPECT_FALSE(tracker.max_drawdown().has_value());
  // 100 -> 120 -> 90 (down 30) -> 110 -> 130 -> 85 (down 45) -> 140
  const std::vector<std::pair<std::int64_t, std::int64_t>> series{
      {100, 0}, {120, 0}, {90, 30}, {110, 30}, {130, 30}, {85, 45}, {140, 45},
  };
  for (const auto& [equity, drawdown] : series) {
    tracker.observe(Money{equity});
    EXPECT_EQ(tracker.max_drawdown(), Money{drawdown}) << equity;
  }
  EXPECT_EQ(tracker.peak(), Money{140});
  EXPECT_EQ(tracker.observations(), 7U);
}

TEST(ValuationTest, DrawdownHandlesNegativeEquityMonotoneSeriesAndOverflow) {
  EquityTracker falling;
  for (const auto equity : {-10, -25, -40, -30}) {
    falling.observe(Money{equity});
  }
  EXPECT_EQ(falling.peak(), Money{-10});
  EXPECT_EQ(falling.max_drawdown(), Money{30});
  EquityTracker rising;
  for (const auto equity : {-5, 0, 5, 5}) {
    rising.observe(Money{equity});
  }
  EXPECT_EQ(rising.max_drawdown(), Money{0});
  EquityTracker extreme;
  extreme.observe(Money{std::numeric_limits<std::int64_t>::max()});
  EXPECT_THROW(extreme.observe(Money{-2}), RunError);
}

} // namespace
} // namespace markettwin
