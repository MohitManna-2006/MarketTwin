#include "markettwin/exchange/matching_engine.hpp"

#include <array>
#include <gtest/gtest.h>
#include <limits>
#include <type_traits>

namespace markettwin {
namespace {

ExchangeConfig config() {
  return {.symbol = SymbolId{1}, .agents = {AgentId{1}, AgentId{2}, AgentId{3}}};
}

Order limit(std::uint64_t id, Side side, std::int64_t price, std::uint64_t quantity,
            std::uint64_t owner = 1) {
  return {
      .id = OrderId{id},
      .owner = AgentId{owner},
      .symbol = SymbolId{1},
      .side = side,
      .type = OrderType::Limit,
      .limit_price = PriceTicks{price},
      .original_quantity = Quantity{quantity},
      .remaining_quantity = Quantity{quantity},
      .tif = TimeInForce::Gtc,
      .arrival_time = SimTime{10},
      .arrival_sequence = Sequence{id},
  };
}

Order market(std::uint64_t id, Side side, std::uint64_t quantity, std::uint64_t owner = 2) {
  auto order = limit(id, side, 0, quantity, owner);
  order.type = OrderType::Market;
  order.tif = TimeInForce::Ioc;
  return order;
}

void expect_rejected(MatchingEngine& engine, const Order& order, RejectReason reason) {
  const auto before = engine.snapshot(100);
  const auto count = engine.book().size();
  const auto result = engine.submit(order);
  EXPECT_FALSE(result.accepted());
  EXPECT_EQ(result.incoming.reason, reason);
  EXPECT_TRUE(result.trades.empty());
  EXPECT_EQ(result.incoming.filled_quantity, Quantity{});
  EXPECT_EQ(result.incoming.resting_quantity, Quantity{});
  EXPECT_EQ(result.incoming.canceled_quantity, Quantity{});
  ASSERT_EQ(result.order_updates.size(), 1U);
  EXPECT_EQ(result.order_updates.front(), result.incoming);
  EXPECT_EQ(engine.snapshot(100), before);
  EXPECT_EQ(engine.book().size(), count);
  EXPECT_NO_THROW(engine.verify_invariants());
}

TEST(StrongTypesTest, DistinctTypesAndCheckedQuantityBoundaries) {
  static_assert(!std::is_convertible_v<PriceTicks, Quantity>);
  static_assert(!std::is_convertible_v<OrderId, AgentId>);
  static_assert(!std::is_convertible_v<std::uint64_t, OrderId>);
  static_assert(!std::is_copy_constructible_v<MatchingEngine>);
  static_assert(!std::is_move_constructible_v<MatchingEngine>);
  const Quantity maximum{std::numeric_limits<std::uint64_t>::max()};
  EXPECT_EQ(checked_add(maximum, Quantity{}), maximum);
  EXPECT_EQ(checked_subtract(maximum, maximum), Quantity{});
  EXPECT_EQ(checked_add(Quantity{3}, Quantity{4}), Quantity{7});
  EXPECT_THROW(static_cast<void>(checked_add(maximum, Quantity{1})), std::overflow_error);
  EXPECT_THROW(static_cast<void>(checked_subtract(Quantity{}, Quantity{1})), std::underflow_error);
}

TEST(ExchangeTest, EmptyBookAndSnapshotBounds) {
  const MatchingEngine engine{config()};
  EXPECT_FALSE(engine.best_bid());
  EXPECT_FALSE(engine.best_ask());
  EXPECT_FALSE(engine.book().find(OrderId{1}));
  EXPECT_TRUE(engine.snapshot(1).bids.empty());
  EXPECT_TRUE(engine.snapshot(1).asks.empty());
  for (const auto depth : {0U, 1001U}) {
    try {
      static_cast<void>(engine.snapshot(depth));
      FAIL() << "expected depth rejection";
    } catch (const ExchangeError& error) {
      EXPECT_EQ(error.reason(), RejectReason::InvalidDepth);
    }
  }
  EXPECT_NO_THROW(engine.verify_invariants());
}

TEST(ExchangeTest, InvalidConfigurationsHaveStructuredErrors) {
  std::array<ExchangeConfig, 9> invalid;
  invalid.fill(config());
  invalid.at(0).symbol = SymbolId{};
  invalid.at(1).agents.clear();
  invalid.at(2).agents.emplace_back();
  invalid.at(3).agents.emplace_back(1);
  invalid.at(4).max_order_quantity = Quantity{};
  invalid.at(5).max_active_orders = 0;
  invalid.at(6).max_accepted_orders = 0;
  invalid.at(7).max_trades = 0;
  invalid.at(8).max_snapshot_depth = 0;
  for (const auto& settings : invalid) {
    try {
      const MatchingEngine engine{settings};
      FAIL() << "expected config rejection";
    } catch (const ExchangeError& error) {
      EXPECT_EQ(error.reason(), RejectReason::InvalidConfig);
    }
  }
}

TEST(ExchangeTest, InvalidOrdersDoNotMutateOrReserveId) {
  MatchingEngine engine{config()};
  const auto good = limit(1, Side::Buy, 100, 10);
  const auto check = [&](auto modify, RejectReason reason) {
    auto order = good;
    modify(order);
    expect_rejected(engine, order, reason);
  };
  check([](Order& o) { o.id = OrderId{}; }, RejectReason::InvalidOrderId);
  check([](Order& o) { o.symbol = SymbolId{2}; }, RejectReason::UnknownSymbol);
  check([](Order& o) { o.owner = AgentId{}; }, RejectReason::InvalidOwner);
  check([](Order& o) { o.owner = AgentId{99}; }, RejectReason::InvalidOwner);
  // Deliberately malformed enum exercises boundary validation.
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  check([](Order& o) { o.side = static_cast<Side>(99); }, RejectReason::InvalidSide);
  // Deliberately malformed enum exercises boundary validation.
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  check([](Order& o) { o.type = static_cast<OrderType>(99); }, RejectReason::InvalidOrderType);
  // Deliberately malformed enum exercises boundary validation.
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  check([](Order& o) { o.tif = static_cast<TimeInForce>(99); }, RejectReason::InvalidTimeInForce);
  check([](Order& o) { o.limit_price = PriceTicks{}; }, RejectReason::InvalidPrice);
  check([](Order& o) { o.limit_price = PriceTicks{-1}; }, RejectReason::InvalidPrice);
  check([](Order& o) { o.original_quantity = Quantity{}; }, RejectReason::InvalidQuantity);
  check([](Order& o) { o.remaining_quantity = Quantity{9}; }, RejectReason::InvalidQuantity);
  check([](Order& o) { o.original_quantity = o.remaining_quantity = Quantity{1'000'001}; },
        RejectReason::QuantityLimit);
  check([](Order& o) { o.arrival_sequence = Sequence{}; }, RejectReason::InvalidArrival);
  auto bad_market = market(1, Side::Buy, 10);
  bad_market.tif = TimeInForce::Gtc;
  expect_rejected(engine, bad_market, RejectReason::InvalidTimeInForce);
  bad_market.tif = TimeInForce::Ioc;
  bad_market.limit_price = PriceTicks{1};
  expect_rejected(engine, bad_market, RejectReason::InvalidPrice);
  EXPECT_TRUE(engine.submit(good).accepted());
  expect_rejected(engine, good, RejectReason::DuplicateOrderId);
}

TEST(ExchangeTest, ArrivalWatermarkAllowsEqualTimesButRejectsStaleSequenceOrTime) {
  MatchingEngine engine{config()};
  ASSERT_TRUE(engine.submit(limit(1, Side::Buy, 100, 1)).accepted());
  auto order = limit(2, Side::Buy, 100, 1);
  order.arrival_sequence = Sequence{1};
  expect_rejected(engine, order, RejectReason::InvalidArrival);
  order.arrival_sequence = Sequence{2};
  order.arrival_time = SimTime{9};
  expect_rejected(engine, order, RejectReason::InvalidArrival);
  order.arrival_time = SimTime{10};
  EXPECT_TRUE(engine.submit(order).accepted());
  order = limit(3, Side::Buy, 100, 1);
  order.arrival_time = SimTime{11};
  order.arrival_sequence = Sequence{2};
  expect_rejected(engine, order, RejectReason::InvalidArrival);
}

class MatchingSidesTest : public ::testing::TestWithParam<Side> {};

TEST_P(MatchingSidesTest, BestPriceThenFifoAndPartialRestingPriority) {
  MatchingEngine engine{config()};
  const auto maker_side = GetParam();
  const auto taker_side = maker_side == Side::Buy ? Side::Sell : Side::Buy;
  const std::int64_t best = maker_side == Side::Buy ? 102 : 98;
  ASSERT_TRUE(engine.submit(limit(1, maker_side, 100, 4)).accepted());
  ASSERT_TRUE(engine.submit(limit(2, maker_side, best, 3)).accepted());
  ASSERT_TRUE(engine.submit(limit(3, maker_side, best, 6)).accepted());
  const auto result = engine.submit(limit(4, taker_side, 100, 5, 2));
  ASSERT_TRUE(result.accepted());
  ASSERT_EQ(result.trades.size(), 2U);
  EXPECT_EQ(result.trades.at(0).resting_order_id, OrderId{2});
  EXPECT_EQ(result.trades.at(0).quantity, Quantity{3});
  EXPECT_EQ(result.trades.at(1).resting_order_id, OrderId{3});
  EXPECT_EQ(result.trades.at(1).quantity, Quantity{2});
  for (const auto& trade : result.trades) {
    EXPECT_EQ(trade.price_ticks, PriceTicks{best});
    EXPECT_EQ(trade.maker_agent_id, AgentId{1});
    EXPECT_EQ(trade.taker_agent_id, AgentId{2});
    EXPECT_EQ(trade.aggressor_order_id, OrderId{4});
    EXPECT_EQ(trade.timestamp, SimTime{10});
    EXPECT_EQ(trade.arrival_sequence, Sequence{4});
    EXPECT_EQ(trade.buy_order_id, taker_side == Side::Buy ? OrderId{4} : trade.resting_order_id);
    EXPECT_EQ(trade.sell_order_id, taker_side == Side::Sell ? OrderId{4} : trade.resting_order_id);
  }
  ASSERT_EQ(result.order_updates.size(), 3U);
  EXPECT_EQ(result.order_updates.at(0).state, OrderState::Filled);
  EXPECT_EQ(result.order_updates.at(1).state, OrderState::PartiallyFilled);
  EXPECT_EQ(result.incoming.state, OrderState::Filled);
  const auto partial = engine.book().find(OrderId{3});
  if (partial.has_value()) {
    EXPECT_EQ(partial->remaining_quantity, Quantity{4});
  } else {
    FAIL() << "expected resting order to exist";
  }
  ASSERT_TRUE(engine.submit(limit(5, maker_side, best, 2)).accepted());
  const auto sweep = engine.submit(market(6, taker_side, 20));
  ASSERT_EQ(sweep.trades.size(), 3U);
  EXPECT_EQ(sweep.trades.at(0).resting_order_id, OrderId{3});
  EXPECT_EQ(sweep.trades.at(1).resting_order_id, OrderId{5});
  EXPECT_EQ(sweep.trades.at(2).resting_order_id, OrderId{1});
  EXPECT_EQ(sweep.trades.at(2).price_ticks, PriceTicks{100});
  EXPECT_EQ(sweep.trades.at(0).id, TradeId{3});
  EXPECT_EQ(sweep.trades.at(2).id, TradeId{5});
  EXPECT_EQ(sweep.trades.at(2).sequence, Sequence{5});
  EXPECT_EQ(sweep.incoming.filled_quantity, Quantity{10});
  EXPECT_EQ(sweep.incoming.canceled_quantity, Quantity{10});
  EXPECT_EQ(engine.book().size(), 0U);
  EXPECT_NO_THROW(engine.verify_invariants());
}

TEST_P(MatchingSidesTest, LimitStopsAtPriceAndGtcRestsRemainder) {
  MatchingEngine engine{config()};
  const auto side = GetParam();
  const auto opposite = side == Side::Buy ? Side::Sell : Side::Buy;
  const std::int64_t outside = side == Side::Buy ? 101 : 99;
  ASSERT_TRUE(engine.submit(limit(1, opposite, 100, 3)).accepted());
  ASSERT_TRUE(engine.submit(limit(2, opposite, outside, 5)).accepted());
  const auto result = engine.submit(limit(3, side, 100, 8, 2));
  ASSERT_EQ(result.trades.size(), 1U);
  EXPECT_EQ(result.incoming.state, OrderState::PartiallyFilled);
  EXPECT_EQ(result.incoming.filled_quantity, Quantity{3});
  EXPECT_EQ(result.incoming.resting_quantity, Quantity{5});
  const auto resting = engine.book().find(OrderId{3});
  if (resting.has_value()) {
    EXPECT_EQ(resting->original_quantity, Quantity{8});
  } else {
    FAIL() << "expected resting order to exist";
  }
  EXPECT_EQ(engine.book().size(), 2U);
  EXPECT_NO_THROW(engine.verify_invariants());
}

TEST_P(MatchingSidesTest, IocAndMarketCancelResidualIncludingEmptyBook) {
  for (const auto type : {OrderType::Limit, OrderType::Market}) {
    MatchingEngine engine{config()};
    const auto side = GetParam();
    const auto opposite = side == Side::Buy ? Side::Sell : Side::Buy;
    auto incoming = type == OrderType::Market ? market(2, side, 8) : limit(2, side, 100, 8, 2);
    incoming.tif = TimeInForce::Ioc;
    ASSERT_TRUE(engine.submit(limit(1, opposite, 100, 3)).accepted());
    const auto result = engine.submit(incoming);
    EXPECT_EQ(result.incoming.state, OrderState::Canceled);
    EXPECT_EQ(result.incoming.filled_quantity, Quantity{3});
    EXPECT_EQ(result.incoming.canceled_quantity, Quantity{5});
    EXPECT_EQ(result.incoming.resting_quantity, Quantity{});
    incoming.id = OrderId{3};
    incoming.arrival_sequence = Sequence{3};
    const auto empty = engine.submit(incoming);
    EXPECT_EQ(empty.incoming.canceled_quantity, Quantity{8});
    EXPECT_TRUE(empty.trades.empty());
    EXPECT_EQ(engine.book().size(), 0U);
    EXPECT_NO_THROW(engine.verify_invariants());
  }
}

TEST_P(MatchingSidesTest, SelfTradeRejectsBeforeEarlierExternalFills) {
  MatchingEngine engine{config()};
  const auto side = GetParam();
  const auto opposite = side == Side::Buy ? Side::Sell : Side::Buy;
  ASSERT_TRUE(engine.submit(limit(1, opposite, 100, 2, 1)).accepted());
  ASSERT_TRUE(engine.submit(limit(2, opposite, 100, 2, 2)).accepted());
  expect_rejected(engine, market(3, side, 3), RejectReason::SelfTrade);
  const auto before_own = engine.submit(market(3, side, 2));
  ASSERT_EQ(before_own.trades.size(), 1U);
  EXPECT_EQ(before_own.trades.at(0).id, TradeId{1});
  const auto own = engine.book().find(OrderId{2});
  if (own.has_value()) {
    EXPECT_EQ(own->remaining_quantity, Quantity{2});
  } else {
    FAIL() << "expected resting order to exist";
  }
  expect_rejected(engine, market(4, side, 1), RejectReason::SelfTrade);
  const auto outside = limit(4, side, side == Side::Buy ? 99 : 101, 1, 2);
  EXPECT_TRUE(engine.submit(outside).accepted());
  EXPECT_NO_THROW(engine.verify_invariants());
}

TEST_P(MatchingSidesTest, LimitSweepsSeveralPricesAtRestingPrices) {
  MatchingEngine engine{config()};
  const auto side = GetParam();
  const auto opposite = side == Side::Buy ? Side::Sell : Side::Buy;
  for (std::uint64_t id = 1; id <= 4; ++id) {
    ASSERT_TRUE(
        engine.submit(limit(id, opposite, 98 + static_cast<std::int64_t>(id), 2)).accepted());
  }
  const auto boundary = side == Side::Buy ? 101 : 100;
  const auto report = engine.submit(limit(5, side, boundary, 10, 2));
  ASSERT_EQ(report.trades.size(), 3U);
  EXPECT_EQ(report.incoming.filled_quantity, Quantity{6});
  EXPECT_EQ(report.incoming.resting_quantity, Quantity{4});
  for (std::size_t index = 0; index < 3; ++index) {
    const auto expected_price = side == Side::Buy ? 99 + static_cast<std::int64_t>(index)
                                                  : 102 - static_cast<std::int64_t>(index);
    EXPECT_EQ(report.trades.at(index).price_ticks, PriceTicks{expected_price});
  }
  EXPECT_NO_THROW(engine.verify_invariants());
}

TEST(ExchangeTest, IndexHandlesSurviveGrowthCancelsAndLevelRecreation) {
  MatchingEngine engine{config()};
  constexpr std::uint64_t kCount = 2048;
  for (std::uint64_t id = 1; id <= kCount; ++id) {
    ASSERT_TRUE(engine.submit(limit(id, Side::Sell, 100 + static_cast<std::int64_t>(id % 16), 2))
                    .accepted());
  }
  for (std::uint64_t id = 1; id <= kCount; id += 2) {
    ASSERT_TRUE(engine.cancel(OrderId{id}, AgentId{1}).canceled());
    ASSERT_FALSE(engine.book().find(OrderId{id}));
  }
  EXPECT_NO_THROW(engine.verify_invariants());
  const auto report = engine.submit(market(kCount + 1, Side::Buy, kCount));
  ASSERT_EQ(report.trades.size(), kCount / 2);
  for (const auto& trade : report.trades) {
    EXPECT_EQ(trade.resting_order_id.value() % 2, 0U);
  }
  EXPECT_EQ(engine.book().size(), 0U);
  EXPECT_NO_THROW(engine.verify_invariants());
  ASSERT_TRUE(engine.submit(limit(kCount + 2, Side::Sell, 100, 1)).accepted());
  EXPECT_TRUE(engine.cancel(OrderId{kCount + 2}, AgentId{1}).canceled());
  EXPECT_NO_THROW(engine.verify_invariants());
}

INSTANTIATE_TEST_SUITE_P(BothSides, MatchingSidesTest, ::testing::Values(Side::Buy, Side::Sell),
                         [](const ::testing::TestParamInfo<Side>& parameter) {
                           return parameter.param == Side::Buy ? "Buy" : "Sell";
                         });

TEST(ExchangeTest, SnapshotSortsAggregatesTruncatesAndOwnsItsValues) {
  MatchingEngine engine{config()};
  ASSERT_TRUE(engine.submit(limit(1, Side::Buy, 98, 3)).accepted());
  ASSERT_TRUE(engine.submit(limit(2, Side::Buy, 99, 4)).accepted());
  ASSERT_TRUE(engine.submit(limit(3, Side::Buy, 99, 5)).accepted());
  ASSERT_TRUE(engine.submit(limit(4, Side::Sell, 102, 7)).accepted());
  ASSERT_TRUE(engine.submit(limit(5, Side::Sell, 101, 6)).accepted());
  const auto saved = engine.snapshot(2);
  EXPECT_EQ(saved.bids, (std::vector<BookLevel>{{PriceTicks{99}, Quantity{9}, 2},
                                                {PriceTicks{98}, Quantity{3}, 1}}));
  EXPECT_EQ(saved.asks, (std::vector<BookLevel>{{PriceTicks{101}, Quantity{6}, 1},
                                                {PriceTicks{102}, Quantity{7}, 1}}));
  EXPECT_EQ(engine.snapshot(1).bids.size(), 1U);
  EXPECT_EQ(engine.snapshot(1).asks.size(), 1U);
  EXPECT_TRUE(engine.cancel(OrderId{2}, AgentId{1}).canceled());
  EXPECT_EQ(saved.bids.at(0).quantity, Quantity{9});
  EXPECT_EQ(engine.snapshot(2).bids.at(0).quantity, Quantity{5});
  EXPECT_EQ(engine.best_bid(), PriceTicks{99});
  EXPECT_EQ(engine.best_ask(), PriceTicks{101});
}

TEST(ExchangeTest, CancelEveryQueuePositionAndEnforceOwnership) {
  for (const auto side : {Side::Buy, Side::Sell}) {
    for (const auto victim : {1U, 2U, 3U}) {
      MatchingEngine engine{config()};
      for (std::uint64_t id = 1; id <= 3; ++id) {
        ASSERT_TRUE(engine.submit(limit(id, side, 100, 3)).accepted());
      }
      EXPECT_EQ(engine.cancel(OrderId{victim}, AgentId{2}).order.reason, RejectReason::NotOwner);
      EXPECT_EQ(engine.cancel(OrderId{victim}, AgentId{99}).order.reason,
                RejectReason::InvalidOwner);
      const auto result = engine.cancel(OrderId{victim}, AgentId{1});
      EXPECT_TRUE(result.canceled());
      EXPECT_EQ(result.order.canceled_quantity, Quantity{3});
      EXPECT_EQ(engine.cancel(OrderId{victim}, AgentId{1}).order.reason,
                RejectReason::OrderNotFound);
      EXPECT_EQ(engine.cancel(OrderId{99}, AgentId{1}).order.reason, RejectReason::OrderNotFound);
      const auto opposite = side == Side::Buy ? Side::Sell : Side::Buy;
      const auto sweep = engine.submit(market(4, opposite, 6));
      ASSERT_EQ(sweep.trades.size(), 2U);
      for (const auto& trade : sweep.trades) {
        EXPECT_NE(trade.resting_order_id, OrderId{victim});
      }
      EXPECT_EQ(engine.book().size(), 0U);
      EXPECT_NO_THROW(engine.verify_invariants());
    }
  }
}

TEST(ExchangeTest, CancelAfterPartialAndFullFillAndNeverReuseAcceptedIds) {
  MatchingEngine engine{config()};
  ASSERT_TRUE(engine.submit(limit(1, Side::Sell, 100, 5)).accepted());
  ASSERT_TRUE(engine.submit(market(2, Side::Buy, 2)).accepted());
  const auto partial = engine.cancel(OrderId{1}, AgentId{1});
  EXPECT_EQ(partial.order.filled_quantity, Quantity{2});
  EXPECT_EQ(partial.order.canceled_quantity, Quantity{3});
  expect_rejected(engine, limit(1, Side::Sell, 100, 5), RejectReason::DuplicateOrderId);
  expect_rejected(engine, market(2, Side::Buy, 2), RejectReason::DuplicateOrderId);
  EXPECT_EQ(engine.cancel(OrderId{2}, AgentId{2}).order.reason, RejectReason::OrderNotFound);
  ASSERT_TRUE(engine.submit(limit(3, Side::Sell, 100, 5)).accepted());
  ASSERT_TRUE(engine.submit(market(4, Side::Buy, 5)).accepted());
  EXPECT_EQ(engine.cancel(OrderId{3}, AgentId{1}).order.reason, RejectReason::OrderNotFound);
  expect_rejected(engine, limit(3, Side::Sell, 100, 5), RejectReason::DuplicateOrderId);
  ASSERT_TRUE(engine.submit(market(5, Side::Buy, 5)).accepted());
  expect_rejected(engine, market(5, Side::Buy, 5), RejectReason::DuplicateOrderId);
  EXPECT_NO_THROW(engine.verify_invariants());
}

TEST(ExchangeTest, CapacityChecksUsePostMatchStateAndAreTransactional) {
  auto settings = config();
  settings.max_active_orders = 1;
  MatchingEngine engine{settings};
  ASSERT_TRUE(engine.submit(limit(1, Side::Sell, 100, 5)).accepted());
  expect_rejected(engine, limit(2, Side::Buy, 99, 1, 2), RejectReason::ActiveOrderLimit);
  const auto replacement = engine.submit(limit(2, Side::Buy, 100, 8, 2));
  ASSERT_TRUE(replacement.accepted());
  EXPECT_EQ(replacement.incoming.resting_quantity, Quantity{3});
  EXPECT_EQ(engine.book().size(), 1U);
  EXPECT_NO_THROW(engine.verify_invariants());
}

TEST(ExchangeTest, LifetimeOrderAndTradeLimitsRejectWithoutPartialFills) {
  auto settings = config();
  settings.max_accepted_orders = 1;
  MatchingEngine limited{settings};
  ASSERT_TRUE(limited.submit(limit(1, Side::Buy, 100, 1)).accepted());
  EXPECT_TRUE(limited.cancel(OrderId{1}, AgentId{1}).canceled());
  expect_rejected(limited, limit(2, Side::Buy, 100, 1), RejectReason::AcceptedOrderLimit);
  settings = config();
  settings.max_trades = 1;
  MatchingEngine engine{settings};
  ASSERT_TRUE(engine.submit(limit(1, Side::Sell, 100, 2)).accepted());
  ASSERT_TRUE(engine.submit(limit(2, Side::Sell, 100, 2)).accepted());
  expect_rejected(engine, market(3, Side::Buy, 3), RejectReason::TradeLimit);
  const auto first = engine.submit(market(3, Side::Buy, 2));
  ASSERT_EQ(first.trades.size(), 1U);
  EXPECT_EQ(first.trades.at(0).id, TradeId{1});
  expect_rejected(engine, market(4, Side::Buy, 1), RejectReason::TradeLimit);
  EXPECT_TRUE(engine.submit(limit(4, Side::Buy, 99, 1, 2)).accepted());
}

TEST(ExchangeTest, FullWidthQuantitiesAndPricesNeverWrap) {
  auto settings = config();
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  settings.max_order_quantity = Quantity{maximum};
  MatchingEngine engine{settings};
  const auto price = std::numeric_limits<std::int64_t>::max();
  ASSERT_TRUE(engine.submit(limit(1, Side::Sell, price, maximum)).accepted());
  expect_rejected(engine, limit(2, Side::Sell, price, 1), RejectReason::QuantityOverflow);
  const auto result = engine.submit(market(2, Side::Buy, maximum));
  ASSERT_EQ(result.trades.size(), 1U);
  EXPECT_EQ(result.trades.at(0).quantity, Quantity{maximum});
  EXPECT_EQ(result.trades.at(0).price_ticks, PriceTicks{price});
  EXPECT_EQ(result.incoming.filled_quantity, Quantity{maximum});
  EXPECT_TRUE(engine.submit(limit(3, Side::Buy, 1, maximum)).accepted());
  expect_rejected(engine, limit(4, Side::Buy, 1, 1), RejectReason::QuantityOverflow);
  EXPECT_EQ(engine.cancel(OrderId{3}, AgentId{1}).order.canceled_quantity, Quantity{maximum});
  EXPECT_NO_THROW(engine.verify_invariants());
}

TEST(ExchangeTest, MaximumArrivalSequenceIsAcceptedWithoutIncrementOrWrap) {
  MatchingEngine engine{config()};
  auto order = limit(1, Side::Buy, 1, 1);
  order.arrival_sequence = Sequence{std::numeric_limits<std::uint64_t>::max()};
  order.arrival_time = SimTime{std::numeric_limits<std::uint64_t>::max()};
  EXPECT_TRUE(engine.submit(order).accepted());
  order.id = OrderId{2};
  expect_rejected(engine, order, RejectReason::InvalidArrival);
  EXPECT_TRUE(engine.cancel(OrderId{1}, AgentId{1}).canceled());
}

} // namespace
} // namespace markettwin
