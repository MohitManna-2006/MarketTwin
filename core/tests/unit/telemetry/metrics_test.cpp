#include "markettwin/exchange/matching_engine.hpp"
#include "markettwin/telemetry/metrics.hpp"

#include <gtest/gtest.h>

namespace markettwin {
namespace {

// A real exchange feeding the collector the way the kernel does.
struct Market {
  [[nodiscard]] TopOfBook top() const {
    return {.bid = engine.book().top(Side::Buy), .ask = engine.book().top(Side::Sell)};
  }
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): time, owner, side, price, quantity.
  ExecutionReport submit(std::uint64_t now, std::uint64_t owner, Side side, std::int64_t price,
                         std::uint64_t quantity, TimeInForce tif = TimeInForce::Gtc) {
    const auto before = top();
    ++sequence;
    const auto report = engine.submit({
        .id = OrderId{sequence},
        .owner = AgentId{owner},
        .symbol = SymbolId{1},
        .side = side,
        .type = OrderType::Limit,
        .limit_price = PriceTicks{price},
        .original_quantity = Quantity{quantity},
        .remaining_quantity = Quantity{quantity},
        .tif = tif,
        .arrival_time = SimTime{now},
        .arrival_sequence = Sequence{sequence},
    });
    analytics.on_order(before, report);
    analytics.on_book(SimTime{now}, top());
    return report;
  }
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): time, order, owner.
  void cancel(std::uint64_t now, std::uint64_t order, std::uint64_t owner) {
    analytics.on_cancel(engine.cancel(OrderId{order}, AgentId{owner}));
    analytics.on_book(SimTime{now}, top());
  }
  void sample(std::size_t depth = 5) { analytics.on_sample(engine.snapshot(depth), depth); }

  std::vector<AgentId> agents{AgentId{1}, AgentId{2}, AgentId{3}};
  MarketAnalytics analytics{agents};
  MatchingEngine engine{ExchangeConfig{.symbol = SymbolId{1}, .agents = agents}};
  std::uint64_t sequence = 0;
};

OrderBookSnapshot book(std::optional<std::int64_t> bid, std::optional<std::int64_t> ask) {
  OrderBookSnapshot snapshot{.symbol = SymbolId{1}, .bids = {}, .asks = {}};
  if (bid) {
    snapshot.bids.push_back(
        {.price = PriceTicks{*bid}, .quantity = Quantity{10}, .order_count = 1});
  }
  if (ask) {
    snapshot.asks.push_back(
        {.price = PriceTicks{*ask}, .quantity = Quantity{10}, .order_count = 1});
  }
  return snapshot;
}

// One scripted session used by several tests:
//   t=0   agent 1 offers 10 at 102            ask only
//   t=10  agent 2 bids 10 at 100              two-sided, spread 2, midpoint 101
//   t=20  sample
//   t=30  agent 3 buys 4 at 102 (IOC)         trades 4 at 102
//   t=40  agent 3 sells 10 at 100             trades 10 at 100; the bid is gone
//   t=50  sample
//   t=60  agent 1 cancels its remaining 6     empty book
//   t=70  agent 2 sends an order for zero shares (rejected), and cancels nothing
//   t=100 end
void session(Market& market) {
  static_cast<void>(market.submit(0, 1, Side::Sell, 102, 10));
  static_cast<void>(market.submit(10, 2, Side::Buy, 100, 10));
  market.sample();
  static_cast<void>(market.submit(30, 3, Side::Buy, 102, 4, TimeInForce::Ioc));
  static_cast<void>(market.submit(40, 3, Side::Sell, 100, 10));
  market.sample();
  market.cancel(60, 1, 1);
  static_cast<void>(market.submit(70, 2, Side::Buy, 100, 0));
  market.cancel(70, 1, 1);
  market.analytics.finish(SimTime{100});
}

TEST(MarketAnalyticsTest, VolumeTradeCountAndVwapComeFromEveryExecution) {
  Market market;
  session(market);
  const auto metrics = market.analytics.metrics();
  EXPECT_EQ(metrics.trade_count, 2U);
  EXPECT_EQ(metrics.volume, Quantity{14});
  EXPECT_EQ(metrics.vwap_milliticks, 100'571); // (4 * 102 + 10 * 100) / 14 = 100.5714...
  EXPECT_EQ(metrics.last_trade_price, PriceTicks{100});
  EXPECT_EQ(metrics.high_trade_price, PriceTicks{102});
  EXPECT_EQ(metrics.low_trade_price, PriceTicks{100});
}

TEST(MarketAnalyticsTest, SpreadAndDepthAverageOverTheSamplesThatHaveThem) {
  Market market;
  session(market);
  const auto metrics = market.analytics.metrics();
  EXPECT_EQ(metrics.samples, 2U);
  EXPECT_EQ(metrics.two_sided_samples, 1U);
  EXPECT_EQ(metrics.bid_samples, 1U);
  EXPECT_EQ(metrics.ask_samples, 2U);
  EXPECT_EQ(metrics.mean_spread_milliticks, 2'000);
  EXPECT_EQ(metrics.mean_relative_spread_millibps, 198'020); // 2 / 101 = 198.0198 bps
  EXPECT_EQ(metrics.mean_best_bid_quantity_millishares, 10'000);
  EXPECT_EQ(metrics.mean_best_ask_quantity_millishares, 8'000); // 10 then 6
  EXPECT_EQ(metrics.depth_levels, 5U);
  EXPECT_EQ(metrics.mean_bid_depth_millishares, 5'000); // 10 then an empty side
  EXPECT_EQ(metrics.mean_ask_depth_millishares, 8'000);
  // One two-sided sample gives no return, so there is no volatility to report.
  EXPECT_EQ(metrics.return_observations, 0U);
  EXPECT_FALSE(metrics.midpoint_return_volatility_ppb.has_value());
}

TEST(MarketAnalyticsTest, TimeInEachBookStateIsExactAndSumsToTheRun) {
  Market market;
  session(market);
  const auto metrics = market.analytics.metrics();
  EXPECT_EQ(metrics.ask_only_ns, 30U); // 0-10 and 40-60
  EXPECT_EQ(metrics.two_sided_ns, 30U);
  EXPECT_EQ(metrics.bid_only_ns, 0U);
  EXPECT_EQ(metrics.empty_ns, 40U);
  EXPECT_EQ(metrics.total_ns, 100U);
}

TEST(MarketAnalyticsTest, OrderFlowCountsHaveExplicitDenominators) {
  Market market;
  session(market);
  const auto metrics = market.analytics.metrics();
  EXPECT_EQ(metrics.orders_submitted, 5U);
  EXPECT_EQ(metrics.orders_accepted, 4U);
  EXPECT_EQ(metrics.orders_rejected, 1U);
  ASSERT_EQ(metrics.rejections.size(), 1U);
  EXPECT_EQ(metrics.rejections.front(),
            (std::pair{RejectReason::InvalidQuantity, std::uint64_t{1}}));
  EXPECT_EQ(metrics.cancels_requested, 2U);
  EXPECT_EQ(metrics.cancels_succeeded, 1U);
  EXPECT_EQ(metrics.submitted_quantity, Quantity{34});
  EXPECT_EQ(metrics.filled_quantity, Quantity{28}); // both sides of 14 shares
  EXPECT_EQ(metrics.canceled_quantity, Quantity{6});
  EXPECT_EQ(metrics.expired_quantity, Quantity{0});
  EXPECT_EQ(metrics.fill_rate_ppm, 823'529U); // 28 / 34

  const auto& flows = market.analytics.flows();
  ASSERT_EQ(flows.size(), 3U);
  const auto& maker = flows.at(0);
  EXPECT_EQ(maker.orders_accepted, 1U);
  EXPECT_EQ(maker.maker_fills, 1U);
  EXPECT_EQ(maker.maker_quantity, Quantity{4});
  EXPECT_EQ(maker.canceled_quantity, Quantity{6});
  EXPECT_EQ(maker.cancels_requested, 2U);
  EXPECT_EQ(maker.cancels_succeeded, 1U);
  const auto& bidder = flows.at(1);
  EXPECT_EQ(bidder.orders_submitted, 2U);
  EXPECT_EQ(bidder.orders_rejected, 1U);
  EXPECT_EQ(bidder.submitted_quantity, Quantity{10});
  EXPECT_EQ(bidder.maker_quantity, Quantity{10});
  const auto& taker = flows.at(2);
  EXPECT_EQ(taker.taker_fills, 2U);
  EXPECT_EQ(taker.taker_quantity, Quantity{14});
  EXPECT_EQ(taker.maker_fills, 0U);
  // Each accepted share ends up filled, canceled, expired, or resting; nothing rests here.
  for (const auto& flow : flows) {
    const auto accounted = flow.maker_quantity.value() + flow.taker_quantity.value() +
                           flow.canceled_quantity.value() + flow.expired_quantity.value();
    EXPECT_EQ(accounted, flow.submitted_quantity.value()) << flow.agent.value();
  }
}

TEST(MarketAnalyticsTest, SpreadCaptureIsMeasuredAgainstTheMidpointTheAggressorMet) {
  Market market;
  session(market);
  const auto& flows = market.analytics.flows();
  // Agent 1 sold 4 at 102 against a midpoint of 101: one tick, or two half ticks, a share.
  EXPECT_EQ(flows.at(0).maker_edge_half_tick_shares, 8);
  EXPECT_EQ(flows.at(0).maker_fills_measured, 1U);
  // Agent 2 bought 10 at 100 against the same midpoint.
  EXPECT_EQ(flows.at(1).maker_edge_half_tick_shares, 20);
  // The taker paid exactly what the two makers earned.
  EXPECT_EQ(flows.at(2).taker_cost_half_tick_shares, 28);
  EXPECT_EQ(flows.at(2).taker_fills_measured, 2U);
  EXPECT_EQ(flows.at(2).maker_edge_half_tick_shares, 0);
}

TEST(MarketAnalyticsTest, AnExpiredIocRemainderAndAOneSidedFillAreCountedNotMeasured) {
  Market market;
  static_cast<void>(market.submit(0, 1, Side::Sell, 102, 5));
  const auto report = market.submit(5, 2, Side::Buy, 102, 8, TimeInForce::Ioc);
  ASSERT_EQ(report.trades.size(), 1U);
  market.analytics.finish(SimTime{5});
  const auto metrics = market.analytics.metrics();
  EXPECT_EQ(metrics.expired_quantity, Quantity{3});
  EXPECT_EQ(metrics.submitted_quantity, Quantity{13});
  EXPECT_EQ(metrics.fill_rate_ppm, 769'231U); // 10 / 13
  const auto& flows = market.analytics.flows();
  EXPECT_EQ(flows.at(1).expired_quantity, Quantity{3});
  // The book had no bid when the order arrived, so there was no midpoint to measure from.
  EXPECT_EQ(flows.at(0).maker_fills, 1U);
  EXPECT_EQ(flows.at(0).maker_fills_measured, 0U);
  EXPECT_EQ(flows.at(0).maker_edge_half_tick_shares, 0);
}

TEST(MarketAnalyticsTest, AnEmptyRunReportsMissingMetricsRatherThanZeros) {
  MarketAnalytics analytics{std::vector<AgentId>{AgentId{1}}};
  analytics.finish(SimTime{0});
  const auto metrics = analytics.metrics();
  EXPECT_EQ(metrics.trade_count, 0U);
  EXPECT_EQ(metrics.volume, Quantity{0});
  EXPECT_EQ(metrics.total_ns, 0U);
  EXPECT_FALSE(metrics.vwap_milliticks.has_value());
  EXPECT_FALSE(metrics.last_trade_price.has_value());
  EXPECT_FALSE(metrics.high_trade_price.has_value());
  EXPECT_FALSE(metrics.fill_rate_ppm.has_value());
  EXPECT_FALSE(metrics.mean_spread_milliticks.has_value());
  EXPECT_FALSE(metrics.mean_relative_spread_millibps.has_value());
  EXPECT_FALSE(metrics.mean_best_bid_quantity_millishares.has_value());
  EXPECT_FALSE(metrics.depth_levels.has_value());
  EXPECT_FALSE(metrics.mean_bid_depth_millishares.has_value());
  EXPECT_FALSE(metrics.midpoint_return_volatility_ppb.has_value());
  EXPECT_TRUE(metrics.rejections.empty());
}

TEST(MarketAnalyticsTest, ZeroVolumeAndEmptyOrOneSidedSamplesNeverDivideByZero) {
  Market market;
  market.sample(); // an empty book
  static_cast<void>(market.submit(10, 1, Side::Buy, 100, 7));
  market.sample(); // bid only
  market.analytics.finish(SimTime{30});
  const auto metrics = market.analytics.metrics();
  EXPECT_EQ(metrics.samples, 2U);
  EXPECT_EQ(metrics.two_sided_samples, 0U);
  EXPECT_FALSE(metrics.mean_spread_milliticks.has_value());
  EXPECT_FALSE(metrics.mean_relative_spread_millibps.has_value());
  EXPECT_FALSE(metrics.mean_best_ask_quantity_millishares.has_value());
  EXPECT_EQ(metrics.mean_best_bid_quantity_millishares, 7'000); // over the one sample with a bid
  EXPECT_EQ(metrics.mean_bid_depth_millishares, 3'500);         // over both samples
  EXPECT_EQ(metrics.mean_ask_depth_millishares, 0);             // a measured zero, not a gap
  EXPECT_FALSE(metrics.vwap_milliticks.has_value());
  EXPECT_EQ(metrics.fill_rate_ppm, 0U); // shares were submitted and none filled
  EXPECT_EQ(metrics.empty_ns, 10U);
  EXPECT_EQ(metrics.bid_only_ns, 20U);
}

TEST(MarketAnalyticsTest, ReturnVolatilityIsTheSampleDeviationOfConsecutiveMidpointReturns) {
  MarketAnalytics analytics{std::vector<AgentId>{AgentId{1}}};
  // Midpoints 100, 101, 100, 102: returns +1 %, -0.990099 %, +2 %.
  for (const auto ask : {101, 103, 101, 105}) {
    analytics.on_sample(book(99, ask), 1);
  }
  auto metrics = analytics.metrics();
  EXPECT_EQ(metrics.return_observations, 3U);
  EXPECT_EQ(metrics.midpoint_return_volatility_ppb, 15'221'249U);
  // A one-sided sample breaks the chain: the next return needs two fresh midpoints.
  analytics.on_sample(book(99, {}), 1);
  analytics.on_sample(book(99, 101), 1);
  EXPECT_EQ(analytics.metrics().return_observations, 3U);
  analytics.on_sample(book(99, 101), 1);
  metrics = analytics.metrics();
  EXPECT_EQ(metrics.return_observations, 4U);
  EXPECT_EQ(metrics.two_sided_samples, 6U);
  EXPECT_EQ(metrics.samples, 7U);
}

TEST(MarketAnalyticsTest, FewerThanTwoReturnsOrAnAbsurdReturnLeaveVolatilityMissing) {
  MarketAnalytics analytics{std::vector<AgentId>{AgentId{1}}};
  analytics.on_sample(book(99, 101), 1);
  analytics.on_sample(book(99, 103), 1);
  EXPECT_EQ(analytics.metrics().return_observations, 1U);
  EXPECT_FALSE(analytics.metrics().midpoint_return_volatility_ppb.has_value());
  analytics.on_sample(book(99, 105), 1);
  EXPECT_TRUE(analytics.metrics().midpoint_return_volatility_ppb.has_value());
  analytics.on_sample(book(99'000, 105'000), 1); // a thousandfold jump between samples
  EXPECT_FALSE(analytics.metrics().midpoint_return_volatility_ppb.has_value());
}

TEST(MarketAnalyticsTest, MixedSampleDepthsLeaveDepthMeansMissing) {
  Market market;
  static_cast<void>(market.submit(0, 1, Side::Buy, 100, 7));
  market.sample(5);
  market.sample(5);
  EXPECT_EQ(market.analytics.metrics().depth_levels, 5U);
  market.sample(2);
  const auto metrics = market.analytics.metrics();
  EXPECT_FALSE(metrics.depth_levels.has_value());
  EXPECT_FALSE(metrics.mean_bid_depth_millishares.has_value());
  EXPECT_EQ(metrics.mean_best_bid_quantity_millishares, 7'000); // top of book is unaffected
}

TEST(MarketAnalyticsTest, BookStatesAreClassifiedFromTheTopOfBook) {
  const BookLevel level{.price = PriceTicks{1}, .quantity = Quantity{1}, .order_count = 1};
  EXPECT_EQ(book_state({}), BookState::Empty);
  EXPECT_EQ(book_state({.bid = level, .ask = {}}), BookState::BidOnly);
  EXPECT_EQ(book_state({.bid = {}, .ask = level}), BookState::AskOnly);
  EXPECT_EQ(book_state({.bid = level, .ask = level}), BookState::TwoSided);
  MarketAnalytics analytics{std::vector<AgentId>{AgentId{1}}};
  analytics.on_book(SimTime{50}, {.bid = level, .ask = level});
  EXPECT_THROW(analytics.finish(SimTime{49}), RunError);
}

} // namespace
} // namespace markettwin
