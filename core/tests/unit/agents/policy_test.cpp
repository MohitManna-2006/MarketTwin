#include "markettwin/agents/market_maker.hpp"
#include "markettwin/agents/momentum.hpp"
#include "markettwin/agents/noise.hpp"
#include "markettwin/agents/value.hpp"

#include <gtest/gtest.h>

namespace markettwin {
namespace {

// A hand-built observation plus the intents a policy produced from it.
BookLevel level(std::int64_t price, std::uint64_t quantity) {
  return {.price = PriceTicks{price}, .quantity = Quantity{quantity}, .order_count = 1};
}

struct Desk {
  explicit Desk(std::uint64_t seed = 1) : rng{seed, RngStream::AgentPolicy} {
    observation.horizon = SimTime{1'000'000};
    observation.reference_price = PriceTicks{15'000};
    observation.limits = {
        .max_position = 1'000,
        .max_order_quantity = Quantity{100},
        .max_open_orders = 16,
        .max_messages = 1'000,
    };
  }
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): bid then ask, as on a quote screen.
  Desk& quote(std::int64_t bid, std::int64_t ask) {
    MarketDataUpdate update;
    update.best_bid = level(bid, 50);
    update.best_ask = level(ask, 50);
    observation.market = update;
    return *this;
  }
  Desk& working(std::uint64_t id, Side side, std::int64_t price, std::uint64_t quantity,
                bool cancel_requested = false) {
    orders.push_back({
        .id = OrderId{id},
        .side = side,
        .type = OrderType::Limit,
        .price = PriceTicks{price},
        .remaining = Quantity{quantity},
        .tif = TimeInForce::Gtc,
        .sent_at = {},
        .acknowledged = true,
        .cancel_requested = cancel_requested,
    });
    return *this;
  }
  std::vector<Intent> timer(ITradingAgent& agent) {
    observation.open_orders = orders;
    std::vector<Intent> intents;
    AgentActions actions{rng, intents};
    agent.on_timer(observation, actions);
    return intents;
  }
  AgentObservation observation;
  std::vector<WorkingOrder> orders;
  Rng rng;
};

std::vector<SubmitIntent> submits(const std::vector<Intent>& intents, Side side) {
  std::vector<SubmitIntent> result;
  for (const auto& intent : intents) {
    if (const auto* submit = std::get_if<SubmitIntent>(&intent);
        submit != nullptr && submit->side == side) {
      result.push_back(*submit);
    }
  }
  return result;
}

std::vector<std::uint64_t> cancels(const std::vector<Intent>& intents) {
  std::vector<std::uint64_t> result;
  for (const auto& intent : intents) {
    if (const auto* cancel = std::get_if<CancelIntent>(&intent)) {
      result.push_back(cancel->order_id.value());
    }
  }
  return result;
}

std::vector<std::int64_t> prices(const std::vector<SubmitIntent>& orders) {
  std::vector<std::int64_t> result;
  result.reserve(orders.size());
  for (const auto& order : orders) {
    result.push_back(order.limit_price.value());
  }
  return result;
}

constexpr MarketMakerParams kTwoLevels{
    .half_spread_ticks = 2,
    .quote_size = Quantity{100},
    .levels = 2,
    .level_spacing_ticks = 1,
};

TEST(MarketMakerTest, QuotesALadderAroundTheMidAndRoundsAwayFromIt) {
  MarketMaker maker{kTwoLevels};
  Desk desk;
  const auto intents = desk.quote(100, 103).timer(maker); // mid 101.5
  EXPECT_EQ(prices(submits(intents, Side::Buy)), (std::vector<std::int64_t>{99, 98}));
  EXPECT_EQ(prices(submits(intents, Side::Sell)), (std::vector<std::int64_t>{104, 105}));
  for (const auto& intent : intents) {
    const auto& order = std::get<SubmitIntent>(intent);
    EXPECT_EQ(order.quantity, Quantity{100});
    EXPECT_EQ(order.type, OrderType::Limit);
    EXPECT_EQ(order.tif, TimeInForce::Gtc);
  }
}

TEST(MarketMakerTest, FallsBackToLastTradeThenToTheScenarioReference) {
  MarketMaker maker{kTwoLevels};
  Desk empty;
  EXPECT_EQ(prices(submits(empty.timer(maker), Side::Buy)),
            (std::vector<std::int64_t>{14'998, 14'997}));
  Desk one_sided;
  MarketDataUpdate update;
  update.best_bid = level(90, 1);
  one_sided.observation.market = update;
  EXPECT_EQ(prices(submits(one_sided.timer(maker), Side::Sell)),
            (std::vector<std::int64_t>{15'002, 15'003}));
  update.last_trade_price = PriceTicks{200};
  one_sided.observation.market = update;
  EXPECT_EQ(prices(submits(one_sided.timer(maker), Side::Sell)),
            (std::vector<std::int64_t>{202, 203}));
}

TEST(MarketMakerTest, KeepsQuotesAlreadyInPlaceAndReplacesOnlyStaleOnes) {
  MarketMaker maker{kTwoLevels};
  Desk desk;
  desk.quote(100, 103)
      .working(1, Side::Buy, 99, 40)           // wanted price, partly filled: keep its queue place
      .working(2, Side::Buy, 97, 100)          // no longer wanted
      .working(3, Side::Sell, 104, 100)        // wanted
      .working(4, Side::Sell, 109, 100, true); // already being canceled
  const auto intents = desk.timer(maker);
  EXPECT_EQ(cancels(intents), (std::vector<std::uint64_t>{2}));
  EXPECT_EQ(prices(submits(intents, Side::Buy)), (std::vector<std::int64_t>{98}));
  EXPECT_EQ(prices(submits(intents, Side::Sell)), (std::vector<std::int64_t>{105}));
}

TEST(MarketMakerTest, InventoryLimitClampsThenRemovesTheGrowingSide) {
  MarketMaker maker{kTwoLevels};
  Desk desk;
  desk.quote(100, 103).observation.position = 870; // room for 130 more
  auto intents = desk.timer(maker);
  ASSERT_EQ(submits(intents, Side::Buy).size(), 2U);
  EXPECT_EQ(submits(intents, Side::Buy).at(1).quantity, Quantity{30});
  EXPECT_EQ(submits(intents, Side::Sell).size(), 2U);

  // Working orders use up room even while a cancel for them is in flight.
  desk.working(9, Side::Buy, 50, 120, true);
  EXPECT_EQ(submits(desk.timer(maker), Side::Buy).at(0).quantity, Quantity{10});

  desk.orders.clear();
  desk.observation.position = 1'000;
  intents = desk.timer(maker);
  EXPECT_TRUE(submits(intents, Side::Buy).empty());
  EXPECT_EQ(submits(intents, Side::Sell).size(), 2U);
  desk.observation.position = -1'000;
  EXPECT_TRUE(submits(desk.timer(maker), Side::Sell).empty());
}

TEST(MarketMakerTest, QuoteSizeControlShrinksOrWithdrawsQuotes) {
  MarketMaker maker{kTwoLevels};
  Desk desk;
  desk.quote(100, 103).working(1, Side::Buy, 99, 100).working(2, Side::Sell, 104, 100);
  desk.observation.controls.quote_size_ppm = 500'000;
  auto intents = desk.timer(maker);
  EXPECT_EQ(cancels(intents), (std::vector<std::uint64_t>{1, 2})); // oversized for the control
  EXPECT_EQ(submits(intents, Side::Buy).at(0).quantity, Quantity{50});

  desk.observation.controls.quote_size_ppm = 0;
  intents = desk.timer(maker);
  EXPECT_EQ(cancels(intents), (std::vector<std::uint64_t>{1, 2}));
  EXPECT_TRUE(submits(intents, Side::Buy).empty());
  EXPECT_TRUE(submits(intents, Side::Sell).empty());
}

TEST(MarketMakerTest, NeverQuotesBelowOneTickOrCrossesItself) {
  MarketMaker maker{{.half_spread_ticks = 5, .quote_size = Quantity{10}, .levels = 3}};
  Desk desk;
  desk.observation.reference_price = PriceTicks{2};
  const auto intents = desk.timer(maker);
  EXPECT_EQ(prices(submits(intents, Side::Buy)), (std::vector<std::int64_t>{1}));
  EXPECT_EQ(prices(submits(intents, Side::Sell)), (std::vector<std::int64_t>{7, 8, 9}));
}

constexpr InventoryMarketMakerParams kInventory{
    .quoting = kTwoLevels,
    .risk_aversion_ppm = 200,
    .volatility_alpha_ppm = 100'000,
    .volatility_floor_milliticks = 500,
    .max_skew_ticks = 20,
};

TEST(InventoryMarketMakerTest, ZeroRiskAversionReproducesTheBaselineMaker) {
  auto params = kInventory;
  params.risk_aversion_ppm = 0;
  InventoryMarketMaker inventory{params};
  MarketMaker baseline{kTwoLevels};
  Desk desk;
  for (const auto position : {0, 400, -700}) {
    for (const auto bid : {100, 103, 97}) {
      desk.quote(bid, bid + 3).observation.position = position;
      EXPECT_EQ(desk.timer(inventory), desk.timer(baseline));
    }
  }
}

TEST(InventoryMarketMakerTest, SkewsAgainstInventorySymmetrically) {
  // Floor variance 250000 * 200 ppm = 50 milliticks per share at the full horizon.
  Desk desk;
  desk.quote(100, 102);
  const auto target = [&](std::int64_t position) {
    InventoryMarketMaker maker{kInventory};
    desk.observation.position = position;
    return maker.target(desk.observation);
  };
  const auto flat = target(0);
  EXPECT_EQ(flat.reservation_milliticks, 101'000);
  EXPECT_EQ(target(100).reservation_milliticks, 101'000 - 5'000);
  EXPECT_EQ(target(-100).reservation_milliticks, 101'000 + 5'000);
  // One quote lot of risk widens each side: 50 * 100 / 2.
  EXPECT_EQ(flat.half_spread_milliticks, 2'000 + 2'500);
  // Position and shift are both capped.
  EXPECT_EQ(target(1'000).reservation_milliticks, 101'000 - 20'000);
  EXPECT_EQ(target(50'000).reservation_milliticks, 101'000 - 20'000);

  InventoryMarketMaker maker{kInventory};
  desk.observation.position = 100;
  const auto intents = desk.timer(maker); // reservation 96, half spread 4.5
  EXPECT_EQ(prices(submits(intents, Side::Buy)), (std::vector<std::int64_t>{91, 90}));
  EXPECT_EQ(prices(submits(intents, Side::Sell)), (std::vector<std::int64_t>{101, 102}));
}

TEST(InventoryMarketMakerTest, RiskGrowsWithVolatilityAndFadesTowardTheHorizon) {
  Desk desk;
  desk.observation.position = 10;
  InventoryMarketMaker calm{kInventory};
  InventoryMarketMaker volatile_market{kInventory};
  QuoteTarget calm_target;
  QuoteTarget volatile_target;
  for (int step = 0; step < 50; ++step) {
    calm_target = calm.target(desk.quote(100, 102).observation);
    const auto swing = step % 2 == 0 ? 0 : 6;
    volatile_target = volatile_market.target(desk.quote(100 + swing, 102 + swing).observation);
  }
  EXPECT_EQ(calm.variance_milliticks2(), 0U); // the floor applies instead
  EXPECT_GT(volatile_market.variance_milliticks2(), 250'000U);
  EXPECT_GT(volatile_target.half_spread_milliticks, calm_target.half_spread_milliticks);

  InventoryMarketMaker late{kInventory};
  desk.quote(100, 102).observation.now = SimTime{500'000}; // half the horizon left
  EXPECT_EQ(late.target(desk.observation).reservation_milliticks, 101'000 - 250);
  desk.observation.now = desk.observation.horizon;
  const auto at_horizon = late.target(desk.observation);
  EXPECT_EQ(at_horizon.reservation_milliticks, 101'000);
  EXPECT_EQ(at_horizon.half_spread_milliticks, 2'000);
}

constexpr MomentumParams kMomentum{
    .short_window = 2,
    .long_window = 4,
    .threshold_milliticks = 500,
    .order_size = Quantity{10},
    .slippage_ticks = 1,
    .cooldown = SimTime{100},
};

TEST(MomentumAgentTest, WaitsForItsWarmupThenFollowsTheTrend) {
  MomentumAgent agent{kMomentum};
  Desk desk;
  for (const auto bid : {100, 101, 102}) {
    EXPECT_TRUE(desk.quote(bid, bid + 2).timer(agent).empty());
    EXPECT_FALSE(agent.signal_milliticks());
  }
  const auto intents = desk.quote(103, 105).timer(agent); // mids 101, 102, 103, 104
  EXPECT_EQ(agent.signal_milliticks(), 1'000);            // 103.5 - 102.5
  ASSERT_EQ(intents.size(), 1U);
  const auto& order = std::get<SubmitIntent>(intents.front());
  EXPECT_EQ(order.side, Side::Buy);
  EXPECT_EQ(order.limit_price, PriceTicks{106});
  EXPECT_EQ(order.quantity, Quantity{10});
  EXPECT_EQ(order.tif, TimeInForce::Ioc);
}

TEST(MomentumAgentTest, SellsADowntrendIgnoresFlatMarketsAndHonorsCooldownAndLimits) {
  MomentumAgent agent{kMomentum};
  Desk desk;
  for (const auto bid : {110, 108, 106}) {
    static_cast<void>(desk.quote(bid, bid + 2).timer(agent));
  }
  auto intents = desk.quote(104, 106).timer(agent);
  ASSERT_EQ(intents.size(), 1U);
  EXPECT_EQ(std::get<SubmitIntent>(intents.front()).side, Side::Sell);
  EXPECT_EQ(std::get<SubmitIntent>(intents.front()).limit_price, PriceTicks{103});

  desk.observation.now = SimTime{50}; // still cooling down
  EXPECT_TRUE(desk.quote(102, 104).timer(agent).empty());
  desk.observation.now = SimTime{200};
  desk.observation.position = -1'000; // no room left to sell
  EXPECT_TRUE(desk.quote(100, 102).timer(agent).empty());
  desk.observation.position = -995;
  intents = desk.quote(98, 100).timer(agent);
  ASSERT_EQ(intents.size(), 1U);
  EXPECT_EQ(std::get<SubmitIntent>(intents.front()).quantity, Quantity{5});

  MomentumAgent flat{kMomentum};
  Desk quiet;
  for (int step = 0; step < 10; ++step) {
    EXPECT_TRUE(quiet.quote(100, 102).timer(flat).empty());
  }
  EXPECT_EQ(flat.signal_milliticks(), 0);
}

TEST(MomentumAgentTest, NeverSamplesTheScenarioReferenceAsIfItWereAMarketPrice) {
  MomentumAgent agent{kMomentum};
  Desk desk;
  for (int step = 0; step < 10; ++step) {
    EXPECT_TRUE(desk.timer(agent).empty());
  }
  EXPECT_FALSE(agent.signal_milliticks());
}

TEST(ValueAgentTest, TradesOnlyWhenTheTouchIsBeyondItsEstimateByTheThreshold) {
  ValueAgent agent{{
      .estimate_noise_milliticks = 0,
      .threshold_milliticks = 3'000,
      .order_size = Quantity{20},
      .slippage_ticks = 5,
  }};
  Desk desk;
  desk.quote(100, 102);
  EXPECT_TRUE(desk.timer(agent).empty()); // no estimate delivered
  desk.observation.fundamental_estimate_milliticks = 104'999;
  EXPECT_TRUE(desk.timer(agent).empty()); // ask 102 + 3 > estimate
  desk.observation.fundamental_estimate_milliticks = 105'500;
  auto intents = desk.timer(agent);
  ASSERT_EQ(intents.size(), 1U);
  auto order = std::get<SubmitIntent>(intents.front());
  EXPECT_EQ(order.side, Side::Buy);
  EXPECT_EQ(order.limit_price, PriceTicks{102}); // floor(105.5 - 3), not ask + slippage
  EXPECT_EQ(order.tif, TimeInForce::Ioc);

  EXPECT_EQ(order.quantity, Quantity{20}); // one threshold of edge

  // Demand grows with the mispricing, up to the agent's order limit.
  desk.observation.fundamental_estimate_milliticks = 111'000; // three thresholds of edge
  EXPECT_EQ(std::get<SubmitIntent>(desk.timer(agent).front()).quantity, Quantity{60});
  desk.observation.fundamental_estimate_milliticks = 120'000;
  order = std::get<SubmitIntent>(desk.timer(agent).front());
  EXPECT_EQ(order.limit_price, PriceTicks{107});
  EXPECT_EQ(order.quantity, Quantity{100});

  desk.observation.fundamental_estimate_milliticks = 96'500;
  intents = desk.timer(agent);
  ASSERT_EQ(intents.size(), 1U);
  order = std::get<SubmitIntent>(intents.front());
  EXPECT_EQ(order.side, Side::Sell);
  EXPECT_EQ(order.limit_price, PriceTicks{100}); // ceil(96.5 + 3)

  desk.observation.position = -990;
  EXPECT_EQ(std::get<SubmitIntent>(desk.timer(agent).front()).quantity, Quantity{10});
  desk.observation.market.reset();
  EXPECT_TRUE(desk.timer(agent).empty());
}

constexpr NoiseParams kNoise{
    .buy_probability_ppm = 500'000,
    .market_order_ppm = 300'000,
    .min_quantity = Quantity{5},
    .max_quantity = Quantity{15},
    .limit_offset_ticks = 4,
    .max_resting_orders = 2,
};

TEST(NoiseAgentTest, DrawsSideTypeSizeAndOffsetFromItsConfiguredDistributions) {
  NoiseAgent agent{kNoise};
  Desk desk;
  desk.quote(100, 102);
  int buys = 0;
  int markets = 0;
  constexpr int kWakeups = 20'000;
  for (int wakeup = 0; wakeup < kWakeups; ++wakeup) {
    const auto intents = desk.timer(agent);
    ASSERT_EQ(intents.size(), 1U);
    const auto& order = std::get<SubmitIntent>(intents.front());
    EXPECT_GE(order.quantity, Quantity{5});
    EXPECT_LE(order.quantity, Quantity{15});
    buys += order.side == Side::Buy ? 1 : 0;
    if (order.type == OrderType::Market) {
      ++markets;
      EXPECT_EQ(order.limit_price, PriceTicks{});
      EXPECT_EQ(order.tif, TimeInForce::Ioc);
    } else {
      EXPECT_GE(order.limit_price, PriceTicks{97});
      EXPECT_LE(order.limit_price, PriceTicks{105});
      EXPECT_EQ(order.tif, TimeInForce::Gtc);
    }
  }
  EXPECT_NEAR(buys, 10'000, 300);
  EXPECT_NEAR(markets, 6'000, 300);
}

TEST(NoiseAgentTest, CancelsItsOldestRestingOrderToMakeRoom) {
  NoiseAgent agent{{.market_order_ppm = 0, .max_resting_orders = 2}};
  Desk desk;
  desk.quote(100, 102)
      .working(1, Side::Buy, 99, 5, true)
      .working(2, Side::Buy, 98, 5)
      .working(3, Side::Sell, 104, 5);
  const auto intents = desk.timer(agent);
  EXPECT_EQ(cancels(intents), (std::vector<std::uint64_t>{2}));
  EXPECT_EQ(intents.size(), 2U);
}

TEST(NoiseAgentTest, ConsumesTheSameDrawsWhateverTheMarketLooksLike) {
  NoiseAgent first{kNoise};
  NoiseAgent second{kNoise};
  Desk liquid{9};
  Desk empty{9};
  empty.observation.position = 1'000; // buys are impossible here, so some wakeups send nothing
  for (int wakeup = 0; wakeup < 500; ++wakeup) {
    static_cast<void>(liquid.quote(100 + (wakeup % 7), 110).timer(first));
    static_cast<void>(empty.timer(second));
    ASSERT_EQ(liquid.rng.state(), empty.rng.state());
  }
}

} // namespace
} // namespace markettwin
