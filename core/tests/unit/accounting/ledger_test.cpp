#include "markettwin/accounting/ledger.hpp"

#include "../../test_support.hpp"

#include <gtest/gtest.h>
#include <limits>

namespace markettwin {
namespace {

constexpr auto kMax = std::numeric_limits<std::int64_t>::max();

constexpr FeeSchedule kNoFees;

std::vector<AccountOpening> flat_accounts(std::uint64_t count) {
  std::vector<AccountOpening> openings;
  openings.reserve(count);
  for (std::uint64_t agent = 1; agent <= count; ++agent) {
    openings.push_back({.agent = AgentId{agent}, .cash = {}, .position = 0, .cost_basis = {}});
  }
  return openings;
}

Ledger ledger(const FeeSchedule& fees = kNoFees, std::uint64_t tick_size_minor = 1) {
  return Ledger{flat_accounts(3), fees, tick_size_minor};
}

const Account& account(const Ledger& books, std::uint64_t agent) {
  return books.account(AgentId{agent});
}

std::string error_code(const std::function<void()>& action) {
  try {
    action();
  } catch (const RunError& error) {
    return error.code();
  }
  return "no error";
}

TEST(LedgerTest, ATradeMovesCashAndSharesBetweenBuyerAndSeller) {
  auto books = ledger(kNoFees, 5);
  const auto entry = books.apply(test::trade(1, 1, 2, 100, 10));
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->notional, Money{5'000}); // 100 ticks * 10 shares * 5 minor units per tick
  EXPECT_EQ(entry->buyer, AgentId{1});
  EXPECT_EQ(entry->seller, AgentId{2});
  EXPECT_EQ(account(books, 1).cash, Money{-5'000});
  EXPECT_EQ(account(books, 1).position, 10);
  EXPECT_EQ(account(books, 1).bought, Quantity{10});
  EXPECT_EQ(account(books, 2).cash, Money{5'000});
  EXPECT_EQ(account(books, 2).position, -10);
  EXPECT_EQ(account(books, 2).sold, Quantity{10});
  EXPECT_EQ(account(books, 3), ledger(kNoFees, 5).account(AgentId{3})); // a bystander is untouched
  EXPECT_EQ(account(books, 1).gross_notional, Money{5'000});
  EXPECT_EQ(books.fee_sink(), Money{0});
  EXPECT_EQ(books.last_trade(), TradeId{1});
  books.verify();
  // The maker may be either side; the books only care who bought.
  ASSERT_TRUE(books.apply(test::trade(2, 2, 1, 100, 10, true)).has_value());
  EXPECT_EQ(account(books, 1).cash, Money{0});
  EXPECT_EQ(account(books, 1).position, 0);
  EXPECT_EQ(account(books, 2).position, 0);
}

TEST(LedgerTest, OpeningALongOrShortPositionCarriesItsCostAndRealizesNothing) {
  auto books = ledger();
  static_cast<void>(books.apply(test::trade(1, 1, 2, 100, 10)));
  static_cast<void>(books.apply(test::trade(2, 1, 2, 110, 10))); // both add to their side
  EXPECT_EQ(account(books, 1).position, 20);
  EXPECT_EQ(account(books, 1).cost_basis, Money{2'100});
  EXPECT_EQ(account(books, 1).realized_pnl, Money{0});
  EXPECT_EQ(account(books, 2).position, -20);
  EXPECT_EQ(account(books, 2).cost_basis, Money{-2'100});
  EXPECT_EQ(account(books, 2).realized_pnl, Money{0});
  EXPECT_EQ(account(books, 1).max_abs_position, 20U);
  EXPECT_EQ(account(books, 2).max_abs_position, 20U);
}

TEST(LedgerTest, ReducingAPositionRealizesAgainstAverageCostOnBothSides) {
  auto books = ledger();
  static_cast<void>(books.apply(test::trade(1, 1, 2, 100, 10)));
  static_cast<void>(books.apply(test::trade(2, 2, 1, 110, 4))); // the long sells 4 at 110
  EXPECT_EQ(account(books, 1).position, 6);
  EXPECT_EQ(account(books, 1).cost_basis, Money{600});
  EXPECT_EQ(account(books, 1).realized_pnl, Money{40}); // 4 * (110 - 100)
  EXPECT_EQ(account(books, 1).cash, Money{-560});
  EXPECT_EQ(account(books, 2).position, -6);
  EXPECT_EQ(account(books, 2).cost_basis, Money{-600});
  EXPECT_EQ(account(books, 2).realized_pnl, Money{-40}); // the short covered above its entry
  EXPECT_EQ(account(books, 2).cash, Money{560});
  EXPECT_EQ(account(books, 1).max_abs_position, 10U);
  books.verify();
}

TEST(LedgerTest, ClosingAPositionReleasesAllOfItsBasisAndLeavesCashEqualToRealizedPnl) {
  auto books = ledger();
  static_cast<void>(books.apply(test::trade(1, 1, 2, 100, 10)));
  static_cast<void>(books.apply(test::trade(2, 2, 1, 110, 4)));
  static_cast<void>(books.apply(test::trade(3, 2, 1, 90, 6))); // the long sells its last 6 at 90
  for (const auto agent : {1U, 2U}) {
    EXPECT_EQ(account(books, agent).position, 0);
    EXPECT_EQ(account(books, agent).cost_basis, Money{0});
    EXPECT_EQ(account(books, agent).cash, account(books, agent).realized_pnl);
  }
  EXPECT_EQ(account(books, 1).realized_pnl, Money{-20}); // +40 then 6 * (90 - 100)
  EXPECT_EQ(account(books, 2).realized_pnl, Money{20});
  EXPECT_EQ(account(books, 1).min_cash, Money{-1'000});
  EXPECT_EQ(account(books, 2).min_cash, Money{0});
  books.verify();
}

TEST(LedgerTest, ReversingClosesAtThePriceThenOpensTheRemainderAtThatPrice) {
  auto books = ledger();
  static_cast<void>(books.apply(test::trade(1, 1, 2, 100, 10)));
  static_cast<void>(books.apply(test::trade(2, 2, 1, 120, 15))); // long 10 sells 15: now short 5
  EXPECT_EQ(account(books, 1).position, -5);
  EXPECT_EQ(account(books, 1).realized_pnl, Money{200}); // 10 * (120 - 100)
  EXPECT_EQ(account(books, 1).cost_basis, Money{-600});  // short 5 at 120
  EXPECT_EQ(account(books, 1).cash, Money{800});
  EXPECT_EQ(account(books, 2).position, 5); // short 10 bought 15: now long 5
  EXPECT_EQ(account(books, 2).realized_pnl, Money{-200});
  EXPECT_EQ(account(books, 2).cost_basis, Money{600});
  EXPECT_EQ(account(books, 2).cash, Money{-800});
  EXPECT_EQ(account(books, 1).max_abs_position, 10U);
  books.verify();
}

TEST(LedgerTest, PartialFillsAreBookedOneByOneAndTruncationNeverLeaksPnl) {
  auto books = ledger();
  static_cast<void>(books.apply(test::trade(1, 1, 2, 333, 1)));
  static_cast<void>(books.apply(test::trade(2, 1, 2, 333, 1)));
  static_cast<void>(books.apply(test::trade(3, 1, 2, 334, 1)));
  EXPECT_EQ(account(books, 1).cost_basis, Money{1'000});
  EXPECT_EQ(account(books, 1).fills, 3U);
  // Three sales of one share at 400: a third of 1000 does not divide evenly.
  static_cast<void>(books.apply(test::trade(4, 3, 1, 400, 1)));
  EXPECT_EQ(account(books, 1).realized_pnl, Money{67}); // released 333
  EXPECT_EQ(account(books, 1).cost_basis, Money{667});
  static_cast<void>(books.apply(test::trade(5, 3, 1, 400, 1)));
  EXPECT_EQ(account(books, 1).realized_pnl, Money{134}); // released 333 of 667
  EXPECT_EQ(account(books, 1).cost_basis, Money{334});
  static_cast<void>(books.apply(test::trade(6, 3, 1, 400, 1)));
  EXPECT_EQ(account(books, 1).realized_pnl, Money{200}); // exactly 1200 - 1000
  EXPECT_EQ(account(books, 1).cost_basis, Money{0});
  EXPECT_EQ(account(books, 1).cash, Money{200});
  // The short that sold at 333, 333, 334 is still open and untouched by those sales.
  EXPECT_EQ(account(books, 2).position, -3);
  EXPECT_EQ(account(books, 2).cost_basis, Money{-1'000});
  books.verify();
}

TEST(LedgerTest, FeesAndRebatesAreChargedPerFillAndReconcileWithTheFeeSink) {
  auto books = ledger({.maker_ppm = -1'000, .taker_ppm = 2'500});
  const auto entry = books.apply(test::trade(1, 1, 2, 100, 10)); // agent 1 takes, agent 2 makes
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->buyer_fee, Money{3});   // 2.5 rounds away from zero
  EXPECT_EQ(entry->seller_fee, Money{-1}); // a rebate
  EXPECT_EQ(account(books, 1).cash, Money{-1'003});
  EXPECT_EQ(account(books, 1).fees_paid, Money{3});
  EXPECT_EQ(account(books, 2).cash, Money{1'001});
  EXPECT_EQ(account(books, 2).fees_paid, Money{-1});
  EXPECT_EQ(books.fee_sink(), Money{2});
  // Fees never enter cost basis or realized P&L; they are reported on their own.
  EXPECT_EQ(account(books, 1).cost_basis, Money{1'000});
  static_cast<void>(
      books.apply(test::trade(2, 2, 1, 100, 10, true))); // roles swap at the same price
  EXPECT_EQ(account(books, 1).realized_pnl, Money{0});
  EXPECT_EQ(account(books, 2).realized_pnl, Money{0});
  EXPECT_EQ(account(books, 1).cash, Money{-6}); // paid 3 twice as the taker
  EXPECT_EQ(account(books, 2).cash, Money{2});
  EXPECT_EQ(books.fee_sink(), Money{4});
  const auto totals = books.totals();
  EXPECT_EQ(checked_add(totals.cash, totals.fee_sink), totals.initial_cash);
  EXPECT_EQ(totals.position, 0);
  books.verify();
}

TEST(LedgerTest, ADuplicateExecutionReportIsIgnoredAndAMissingOneIsAnError) {
  auto books = ledger();
  const auto first = test::trade(1, 1, 2, 100, 10);
  ASSERT_TRUE(books.apply(first).has_value());
  const auto after_first = books.accounts();
  EXPECT_FALSE(books.apply(first).has_value());
  EXPECT_EQ(books.accounts(), after_first);
  ASSERT_TRUE(books.apply(test::trade(2, 1, 2, 100, 1)).has_value());
  EXPECT_FALSE(books.apply(first).has_value()); // however late the repeat arrives
  const auto after_second = books.accounts();
  EXPECT_EQ(error_code([&] { static_cast<void>(books.apply(test::trade(4, 1, 2, 100, 1))); }),
            "TradeSequenceGap");
  EXPECT_EQ(books.accounts(), after_second);
  EXPECT_EQ(books.last_trade(), TradeId{2});
}

TEST(LedgerTest, MalformedTradesAreRejectedWithoutTouchingAnyAccount) {
  auto books = ledger();
  const auto before = books.accounts();
  const auto rejected = [&](Trade trade) {
    return error_code([&] { static_cast<void>(books.apply(trade)); });
  };
  EXPECT_EQ(rejected(test::trade(1, 1, 1, 100, 10)), "InvalidTrade"); // one party on both sides
  EXPECT_EQ(rejected(test::trade(1, 1, 9, 100, 10)), "UnknownAccount");
  EXPECT_EQ(rejected(test::trade(1, 1, 2, 100, 0)), "InvalidTrade");
  EXPECT_EQ(rejected(test::trade(1, 1, 2, 0, 10)), "InvalidTrade");
  EXPECT_EQ(rejected(test::trade(1, 1, 2, -5, 10)), "InvalidTrade");
  auto crossed = test::trade(1, 1, 2, 100, 10);
  crossed.aggressor_order_id = crossed.resting_order_id;
  EXPECT_EQ(rejected(crossed), "InvalidTrade");
  auto unnumbered = test::trade(1, 1, 2, 100, 10);
  unnumbered.id = TradeId{};
  EXPECT_EQ(rejected(unnumbered), "InvalidTrade");
  EXPECT_EQ(books.accounts(), before);
  EXPECT_EQ(books.last_trade(), TradeId{});
  EXPECT_TRUE(books.apply(test::trade(1, 1, 2, 100, 10)).has_value());
}

TEST(LedgerTest, OverflowIsRejectedAtomicallyAtTheIntegerBoundary) {
  Ledger books{flat_accounts(4), kNoFees, 1};
  // A notional of exactly the largest 64-bit value is booked.
  ASSERT_TRUE(books.apply(test::trade(1, 1, 2, kMax, 1)).has_value());
  EXPECT_EQ(account(books, 1).cash, Money{-kMax});
  EXPECT_EQ(account(books, 2).cash, Money{kMax});
  const auto before = books.accounts();
  const auto rejected = [&](const Trade& trade) {
    return error_code([&] { static_cast<void>(books.apply(trade)); });
  };
  EXPECT_EQ(rejected(test::trade(2, 1, 2, kMax, 1)), "AccountingOverflow");
  // Agent 3's side of this trade fits and is computed first; agent 1's cash does not fit.
  // Neither side may be booked.
  EXPECT_EQ(rejected(test::trade(2, 1, 3, 2, 1)), "AccountingOverflow");
  EXPECT_EQ(rejected(test::trade(2, 3, 4, (kMax / 2) + 1, 2)), "AccountingOverflow"); // notional
  EXPECT_EQ(rejected(test::trade(2, 3, 4, 1, ~std::uint64_t{0})), "AccountingOverflow");
  EXPECT_EQ(rejected(test::trade(2, 3, 4, 1, 1ULL << 63U)), "AccountingOverflow");
  EXPECT_EQ(books.accounts(), before);
  EXPECT_EQ(books.fee_sink(), Money{0});
  EXPECT_EQ(books.last_trade(), TradeId{1});
  // The ledger is still usable, and the rejected ID was never consumed.
  ASSERT_TRUE(books.apply(test::trade(2, 3, 4, 100, 1)).has_value());
  EXPECT_EQ(account(books, 3).position, 1);
  books.verify();
}

TEST(LedgerTest, SharesAndCashAreConservedRelativeToOpeningBalances) {
  const std::vector<AccountOpening> openings{
      {.agent = AgentId{2}, .cash = Money{-500}, .position = -4, .cost_basis = Money{-380}},
      {.agent = AgentId{1}, .cash = Money{10'000}, .position = 7, .cost_basis = Money{700}},
  };
  Ledger books{openings, {.maker_ppm = 20'000, .taker_ppm = 30'000}, 1};
  EXPECT_EQ(books.accounts().front().agent, AgentId{1}); // sorted by agent
  static_cast<void>(books.apply(test::trade(1, 2, 1, 105, 3)));
  static_cast<void>(books.apply(test::trade(2, 1, 2, 95, 9, true)));
  const auto totals = books.totals();
  EXPECT_EQ(totals.initial_position, 3);
  EXPECT_EQ(totals.position, 3);
  EXPECT_EQ(totals.initial_cash, Money{9'500});
  EXPECT_EQ(checked_add(totals.cash, totals.fee_sink), Money{9'500});
  EXPECT_EQ(totals.fee_sink, Money{9 + 6 + 17 + 26}); // 3 % and 2 % of 315, then of 855
  // Agent 1 sold 3 of its 7 at 105 (opened at 100 each), then bought 9 more at 95.
  EXPECT_EQ(account(books, 1).realized_pnl, Money{15});
  EXPECT_EQ(account(books, 1).position, 13);
  EXPECT_EQ(account(books, 1).cost_basis, Money{400 + 855});
  // Agent 2 covered 3 of its short 4 (opened at 95 each) at 105, then sold 9 at 95.
  EXPECT_EQ(account(books, 2).realized_pnl, Money{-30});
  EXPECT_EQ(account(books, 2).position, -10);
  EXPECT_EQ(account(books, 2).cost_basis, Money{-95 - 855});
  books.verify();
}

TEST(LedgerTest, RejectsInconsistentOpeningsAndUnknownAccounts) {
  const auto build = [](std::vector<AccountOpening> openings, FeeSchedule fees = kNoFees,
                        std::uint64_t tick = 1) {
    return error_code([&] { const Ledger books{std::move(openings), fees, tick}; });
  };
  const AccountOpening flat{.agent = AgentId{1}, .cash = Money{0}, .position = 0, .cost_basis = {}};
  EXPECT_EQ(build({flat}), "no error");
  EXPECT_EQ(build({flat, flat}), "InvalidConfig");
  EXPECT_EQ(build({flat}, kNoFees, 0), "InvalidConfig");
  EXPECT_EQ(build({flat}, {.maker_ppm = 1'000'001, .taker_ppm = 0}), "InvalidConfig");
  EXPECT_EQ(build({{.agent = AgentId{}, .cash = Money{0}, .position = 0, .cost_basis = {}}}),
            "InvalidConfig");
  EXPECT_EQ(build({{.agent = AgentId{1}, .cash = {}, .position = 0, .cost_basis = Money{5}}}),
            "InvalidConfig");
  EXPECT_EQ(build({{.agent = AgentId{1}, .cash = {}, .position = 2, .cost_basis = Money{-5}}}),
            "InvalidConfig");
  EXPECT_EQ(build({{.agent = AgentId{1}, .cash = {}, .position = -2, .cost_basis = Money{5}}}),
            "InvalidConfig");
  EXPECT_EQ(build({{.agent = AgentId{1}, .cash = Money{kMax}, .position = 0, .cost_basis = {}},
                   {.agent = AgentId{2}, .cash = Money{1}, .position = 0, .cost_basis = {}}}),
            "AccountingOverflow");
  EXPECT_EQ(error_code([] { static_cast<void>(ledger().account(AgentId{9})); }), "UnknownAccount");
}

} // namespace
} // namespace markettwin
