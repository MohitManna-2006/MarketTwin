#include "markettwin/common/hash.hpp"
#include "markettwin/telemetry/replay.hpp"

#include "../test_support.hpp"

#include <gtest/gtest.h>
#include <map>
#include <numeric>
#include <set>

namespace markettwin {
namespace {
using Json = nlohmann::json;
using test::kMillisecond;
using test::kSecond;

std::uint64_t time_of(const Json& record) {
  return record.at("timestamp_ns").get<std::uint64_t>();
}

// A lone maker with nobody to trade against: its messages isolate timing and limits.
RunConfig lone_maker() {
  RunConfig config;
  config.run_id = "lone-maker";
  config.exchange.agents = {AgentId{1}};
  config.limits = {.horizon = SimTime{10 * kSecond}, .max_events = 100'000, .max_queue = 1'000};
  ScenarioConfig scenario;
  scenario.fundamental.volatility_milliticks = 0;
  scenario.agents = {test::agent(1, MarketMakerParams{})};
  config.scenario = std::move(scenario);
  return config;
}

TEST(ScenarioRunTest, SameSeedIsByteIdenticalAndADifferentSeedIsADifferentMarket) {
  const auto config = test::scenario_fixture();
  const auto first = test::capture(config);
  const auto second = test::capture(config);
  EXPECT_EQ(first.summary, second.summary);
  EXPECT_EQ(first.records.commands, second.records.commands);
  EXPECT_EQ(first.records.trace, second.records.trace);

  auto reseeded = config;
  reseeded.seed = 8;
  const auto other = test::capture(reseeded);
  EXPECT_NE(other.summary.trade_digest, first.summary.trade_digest);
  EXPECT_NE(test::scenario(other.summary).rng_digest, test::scenario(first.summary).rng_digest);
  EXPECT_NE(test::scenario(other.summary).fundamental_milliticks,
            test::scenario(first.summary).fundamental_milliticks);
}

TEST(ScenarioRunTest, EveryPolicyTradesAndSharesAreConservedAcrossAgents) {
  const auto run = test::capture(test::scenario_fixture());
  const auto& scenario = test::scenario(run.summary);
  EXPECT_EQ(run.summary.stop_reason, StopReason::Horizon);
  EXPECT_EQ(run.summary.final_time, SimTime{60 * kSecond});
  EXPECT_GT(run.summary.trade_count, 100U);
  EXPECT_EQ(scenario.fundamental_updates, 60U);

  std::int64_t net = 0;
  std::uint64_t filled = 0;
  std::uint64_t fills = 0;
  for (const auto& agent : scenario.agents) {
    EXPECT_GT(agent.wakeups, 0U) << agent.policy;
    EXPECT_GT(agent.orders_sent, 0U) << agent.policy;
    EXPECT_GT(agent.fills, 0U) << agent.policy;
    EXPECT_EQ(agent.intents_dropped, 0U) << agent.policy;
    net += agent.net_position;
    filled += agent.filled_quantity.value();
    fills += agent.fills;
  }
  EXPECT_EQ(net, 0); // every trade moves the same shares between two agents
  EXPECT_EQ(fills, 2 * run.summary.trade_count);
  std::uint64_t traded = 0;
  for (const auto& trade : test::CaptureSink::of_kind(run.records.trace, "Trade")) {
    traded += trade.at("payload").at("quantity").get<std::uint64_t>();
  }
  EXPECT_EQ(filled, 2 * traded);

  const auto snapshots = test::CaptureSink::of_kind(run.records.trace, "BookSnapshot");
  ASSERT_EQ(snapshots.size(), 60U);
  for (const auto& snapshot : snapshots) {
    const auto& book = snapshot.at("payload");
    ASSERT_FALSE(book.at("best_bid_ticks").is_null());
    ASSERT_FALSE(book.at("best_ask_ticks").is_null());
    EXPECT_LT(book.at("best_bid_ticks").get<std::int64_t>(),
              book.at("best_ask_ticks").get<std::int64_t>());
  }
}

TEST(ScenarioRunTest, EveryRecordIsCanonicalJsonWithAValidPayloadDigest) {
  const auto run = test::capture(test::scenario_fixture());
  std::uint64_t last_sequence = 0;
  std::set<std::string> kinds;
  for (const auto* lines : {&run.records.commands, &run.records.trace}) {
    last_sequence = 0;
    for (const auto& line : *lines) {
      const auto record = Json::parse(line);
      ASSERT_EQ(record.dump() + '\n', line); // sorted keys, compact, integer-only
      if (record.at("kind") == "Header") {
        continue;
      }
      ASSERT_EQ(record.at("payload_digest"), sha256(record.at("payload").dump()));
      ASSERT_GT(record.at("sequence").get<std::uint64_t>(), last_sequence);
      last_sequence = record.at("sequence").get<std::uint64_t>();
      kinds.insert(record.at("kind").get<std::string>());
    }
  }
  for (const auto* kind : {
           "OrderArrival",
           "CancelArrival",
           "SnapshotRequest",
           "Trade",
           "OrderStatus",
           "CancelResult",
           "BookSnapshot",
           "FundamentalValueUpdate",
           "RunCompleted",
       }) {
    EXPECT_TRUE(kinds.contains(kind)) << kind;
  }
}

TEST(ScenarioRunTest, AgentOrderIdsAreNamespacedAndArrivalsNeverRegress) {
  const auto run = test::capture(test::scenario_fixture());
  std::map<std::uint64_t, std::uint64_t> last_arrival;
  std::map<std::uint64_t, std::uint64_t> last_local_id;
  for (const auto& order : test::CaptureSink::of_kind(run.records.commands, "OrderArrival")) {
    const auto actor = order.at("actor_id").get<std::uint64_t>();
    const auto id = order.at("payload").at("id").get<std::uint64_t>();
    EXPECT_EQ(id >> kAgentOrderIdShift, actor);
    EXPECT_EQ(id & 0xffffffffULL, ++last_local_id[actor]); // FIFO: sent order is arrival order
    EXPECT_GE(time_of(order), last_arrival[actor]);
    last_arrival[actor] = time_of(order);
  }
}

TEST(ScenarioRunTest, OrdersArriveExactlyOneOrderEntryLatencyAfterTheWakeup) {
  auto config = lone_maker();
  test::scenario(config).agents.at(0).latency.order_entry = SimTime{7 * kMillisecond};
  const auto run = test::capture(config);
  const auto orders = test::CaptureSink::of_kind(run.records.commands, "OrderArrival");
  ASSERT_EQ(orders.size(), 2U); // one bid and one ask; nothing ever changes them
  for (const auto& order : orders) {
    EXPECT_EQ(time_of(order), 7 * kMillisecond);
  }
  EXPECT_EQ(test::scenario(run.summary).agents.at(0).cancels_sent, 0U);
  EXPECT_EQ(run.summary.resting_order_count, 2U);
}

TEST(ScenarioRunTest, JitterStaysInItsBoundsAndIsDrawnFromTheLatencyStream) {
  auto config = test::scenario_fixture();
  for (auto& agent : test::scenario(config).agents) {
    agent.latency.order_entry = SimTime{2 * kMillisecond};
    agent.latency.order_entry_jitter = SimTime{kMillisecond};
  }
  const auto jittered = test::capture(config);
  EXPECT_NE(jittered.summary.commands_digest,
            test::capture(test::scenario_fixture()).summary.commands_digest);
  EXPECT_EQ(jittered.summary, test::capture(config).summary);
  // Noise traders decide once per wakeup from draws that ignore the market, so their
  // order count depends only on the wakeup stream, which jitter must not disturb.
  const auto plain = test::capture(test::scenario_fixture());
  for (const std::size_t slot : {4U, 5U}) {
    EXPECT_EQ(test::scenario(jittered.summary).agents.at(slot).wakeups,
              test::scenario(plain.summary).agents.at(slot).wakeups);
    EXPECT_EQ(test::scenario(jittered.summary).agents.at(slot).orders_sent,
              test::scenario(plain.summary).agents.at(slot).orders_sent);
  }
}

TEST(ScenarioRunTest, AnAgentActsOnlyOnMarketDataThatHasBeenDeliveredToIt) {
  // Agent 2 quotes 14998 / 15002 and wakes every 500 ms. At 5.2 s a scripted offer at
  // 15001 improves the ask, which should make it requote. When it does so depends only on
  // when the update reaches it, never on when the book actually changed.
  auto config = lone_maker();
  config.exchange.agents = {AgentId{1}, AgentId{2}};
  test::scenario(config).agents = {test::agent(2, MarketMakerParams(), 500 * kMillisecond)};
  auto offer = test::order(1, Side::Sell, 1, 1);
  offer.limit_price = PriceTicks{15'001};
  config.commands = {test::at(5'200 * kMillisecond, offer)};
  const auto first_reaction = [&](std::uint64_t market_data_latency) {
    auto delayed = config;
    test::scenario(delayed).agents.at(0).latency.market_data = SimTime{market_data_latency};
    const auto run = test::capture(delayed);
    for (const auto& order : test::CaptureSink::of_kind(run.records.commands, "OrderArrival")) {
      if (order.at("actor_id") == 2 && time_of(order) > kSecond) {
        return time_of(order);
      }
    }
    return std::uint64_t{0};
  };
  const auto order_entry = kMillisecond;
  EXPECT_EQ(first_reaction(kMillisecond), (5'500 * kMillisecond) + order_entry);
  EXPECT_EQ(first_reaction(299 * kMillisecond), (5'500 * kMillisecond) + order_entry);
  // Delivered at exactly 5.5 s, but after the wakeup that was already queued for 5.5 s.
  EXPECT_EQ(first_reaction(300 * kMillisecond), (6'000 * kMillisecond) + order_entry);
  EXPECT_EQ(first_reaction(700 * kMillisecond), (6'000 * kMillisecond) + order_entry);
  EXPECT_EQ(first_reaction(3'600 * kSecond), 0U); // never delivered, so never acted on
}

TEST(ScenarioRunTest, ZeroLatencyStillOrdersAnAgentsMessagesAfterEventsAlreadyQueued) {
  auto config = lone_maker();
  test::scenario(config).agents.at(0).latency = {};
  const auto run = test::capture(config);
  const auto orders = test::CaptureSink::of_kind(run.records.commands, "OrderArrival");
  ASSERT_EQ(orders.size(), 2U);
  EXPECT_EQ(time_of(orders.at(0)), 0U); // same timestamp as the wakeup that produced it
  EXPECT_LT(orders.at(0).at("scheduler_sequence").get<std::uint64_t>(),
            orders.at(1).at("scheduler_sequence").get<std::uint64_t>());
}

TEST(ScenarioRunTest, MessageBudgetAndOpenOrderLimitDropIntentsInsteadOfSendingThem) {
  auto config = test::scenario_fixture();
  test::scenario(config).agents.at(4).limits.max_messages = 25;
  const auto run = test::capture(config);
  const auto& capped = test::scenario(run.summary).agents.at(4);
  EXPECT_EQ(capped.orders_sent + capped.cancels_sent, 25U);
  EXPECT_GT(capped.intents_dropped, 0U);
  std::uint64_t arrivals = 0;
  for (const auto* kind : {"OrderArrival", "CancelArrival"}) {
    for (const auto& command : test::CaptureSink::of_kind(run.records.commands, kind)) {
      arrivals += command.at("actor_id") == 5 ? 1U : 0U;
    }
  }
  EXPECT_LE(arrivals, 25U);
}

TEST(ScenarioRunTest, EventCapStopsARunawayScenarioWithARecordedReason) {
  auto config = test::scenario_fixture();
  config.limits.max_events = 1'000;
  const auto run = test::capture(config);
  EXPECT_EQ(run.summary.stop_reason, StopReason::EventLimit);
  EXPECT_EQ(run.summary.event_count, 1'000U);
  EXPECT_LT(run.summary.final_time, SimTime{60 * kSecond});
  config.limits.max_queue = 4;
  EXPECT_THROW(static_cast<void>(test::capture(config)), RunError);
}

TEST(ScenarioRunTest, FundamentalJumpIsLoggedAndPricesFollowThroughOrdersAlone) {
  auto config = test::scenario_fixture();
  config.limits.horizon = SimTime{120 * kSecond};
  const auto calm = test::capture(config);
  test::scenario(config).shocks = {
      FundamentalJump{.at = SimTime{30 * kSecond}, .delta_milliticks = -40'000},
  };
  const auto shocked = test::capture(config);

  const auto shocks = test::CaptureSink::of_kind(shocked.records.trace, "ScenarioShock");
  ASSERT_EQ(shocks.size(), 1U);
  EXPECT_EQ(time_of(shocks.at(0)), 30 * kSecond);
  EXPECT_EQ(shocks.at(0).at("payload").at("phase"), "Instant");
  EXPECT_EQ(shocks.at(0).at("payload").at("shock").at("delta_milliticks"), -40'000);
  EXPECT_TRUE(shocks.at(0).at("actor_id").is_null());
  EXPECT_EQ(test::scenario(shocked.summary).shock_transitions, 1U);
  // The same random path, shifted: the jump adds no draws to the fundamental stream.
  EXPECT_EQ(test::scenario(shocked.summary).fundamental_milliticks,
            test::scenario(calm.summary).fundamental_milliticks - 40'000);

  const auto last_mid = [](const test::CapturedRun& run) {
    const auto book =
        test::CaptureSink::of_kind(run.records.trace, "BookSnapshot").back().at("payload");
    return (book.at("best_bid_ticks").get<std::int64_t>() +
            book.at("best_ask_ticks").get<std::int64_t>()) /
           2;
  };
  EXPECT_LT(last_mid(shocked), last_mid(calm) - 20); // most of a 40-tick jump

  // Before the shock the two runs are the same market. Only scheduler sequences differ,
  // because the queued shock event takes one.
  const auto prefix = [](const test::CapturedRun& run) {
    std::vector<Json> records;
    for (const auto& line : run.records.trace) {
      auto record = Json::parse(line);
      if (record.at("kind") != "Header" && time_of(record) < 30 * kSecond) {
        record.erase("scheduler_sequence");
        records.push_back(std::move(record));
      }
    }
    return records;
  };
  ASSERT_GT(prefix(calm).size(), 100U);
  EXPECT_TRUE(prefix(shocked) == prefix(calm));
}

TEST(ScenarioRunTest, LiquidityWithdrawalSilencesTheNamedMakersUntilItEnds) {
  auto config = test::scenario_fixture();
  test::scenario(config).shocks = {
      LiquidityWithdrawal{
          .start = SimTime{20 * kSecond},
          .end = SimTime{40 * kSecond},
          .agents = {AgentId{1}, AgentId{2}},
          .quote_size_ppm = 0,
      },
  };
  const auto run = test::capture(config);
  const auto shocks = test::CaptureSink::of_kind(run.records.trace, "ScenarioShock");
  ASSERT_EQ(shocks.size(), 2U);
  EXPECT_EQ(shocks.at(0).at("payload").at("phase"), "Begin");
  EXPECT_EQ(shocks.at(1).at("payload").at("phase"), "End");
  EXPECT_EQ(time_of(shocks.at(1)), 40 * kSecond);

  // Makers wake every 500 ms and their messages take 1 ms, so allow one second to react.
  std::uint64_t during = 0;
  std::uint64_t after = 0;
  for (const auto& order : test::CaptureSink::of_kind(run.records.commands, "OrderArrival")) {
    const auto maker = order.at("actor_id") == 1 || order.at("actor_id") == 2;
    during += maker && time_of(order) > 21 * kSecond && time_of(order) <= 40 * kSecond ? 1U : 0U;
    after += maker && time_of(order) > 40 * kSecond ? 1U : 0U;
  }
  EXPECT_EQ(during, 0U);
  EXPECT_GT(after, 0U);

  const auto depth = [&](std::uint64_t from, std::uint64_t to) {
    std::uint64_t total = 0;
    std::uint64_t samples = 0;
    for (const auto& snapshot : test::CaptureSink::of_kind(run.records.trace, "BookSnapshot")) {
      if (time_of(snapshot) > from && time_of(snapshot) <= to) {
        for (const auto* side : {"bids", "asks"}) {
          for (const auto& level : snapshot.at("payload").at(side)) {
            total += level.at("quantity").get<std::uint64_t>();
          }
        }
        ++samples;
      }
    }
    return total / std::max<std::uint64_t>(samples, 1);
  };
  EXPECT_LT(depth(22 * kSecond, 40 * kSecond) * 4, depth(0, 20 * kSecond));
  EXPECT_GT(depth(42 * kSecond, 60 * kSecond) * 2, depth(0, 20 * kSecond));
}

TEST(ScenarioRunTest, VolatilityRegimeScalesFundamentalStepsOnlyInsideItsWindow) {
  auto config = test::scenario_fixture();
  config.limits.horizon = SimTime{600 * kSecond};
  test::scenario(config).agents.clear();
  test::scenario(config).sampling.reset();
  test::scenario(config).fundamental.mean_reversion_ppm = 0;
  test::scenario(config).shocks = {
      VolatilityRegime{
          .start = SimTime{200 * kSecond},
          .end = SimTime{400 * kSecond},
          .volatility_ppm = 5 * kPpm,
      },
  };
  const auto run = test::capture(config);
  const auto updates = test::CaptureSink::of_kind(run.records.trace, "FundamentalValueUpdate");
  ASSERT_EQ(updates.size(), 600U);
  std::int64_t previous = test::scenario(config).fundamental.initial_milliticks;
  std::array<std::uint64_t, 3> squares{};
  for (const auto& update : updates) {
    const auto value = update.at("payload").at("value_milliticks").get<std::int64_t>();
    const auto step = value - previous;
    previous = value;
    // The regime covers [start, end): a shock queued for a tick's timestamp applies first.
    const std::size_t window =
        (time_of(update) >= 200 * kSecond ? 1U : 0U) + (time_of(update) >= 400 * kSecond ? 1U : 0U);
    squares.at(window) += static_cast<std::uint64_t>(step * step);
    EXPECT_EQ(update.at("payload").at("volatility_milliticks"), window == 1 ? 5'000 : 1'000);
  }
  // Variance scales by 25; allow wide sampling error on 200 steps per window.
  EXPECT_GT(squares.at(1), 15 * squares.at(0));
  EXPECT_GT(squares.at(1), 15 * squares.at(2));
  EXPECT_EQ(run.summary.trade_count, 0U);
}

TEST(ScenarioRunTest, OrderFlowImbalanceShiftsTheNamedNoiseTradersOnlyInsideItsWindow) {
  auto config = test::scenario_fixture();
  config.limits.horizon = SimTime{120 * kSecond};
  test::scenario(config).shocks = {
      OrderFlowImbalance{
          .start = SimTime{40 * kSecond},
          .end = SimTime{80 * kSecond},
          .agents = {AgentId{5}, AgentId{6}},
          .buy_probability_ppm = 950'000,
          .market_order_ppm = kPpm,
          .arrival_rate_ppm = 3 * kPpm,
      },
  };
  const auto run = test::capture(config);
  const auto shocks = test::CaptureSink::of_kind(run.records.trace, "ScenarioShock");
  ASSERT_EQ(shocks.size(), 2U);
  EXPECT_EQ(shocks.at(0).at("payload").at("shock").at("kind"), "OrderFlowImbalance");

  struct Window {
    std::uint64_t orders = 0;
    std::uint64_t buys = 0;
    std::uint64_t market = 0;
  };
  // A wakeup queued before a transition keeps its time, so leave two mean intervals of
  // slack after each one.
  std::array<Window, 3> windows{};
  for (const auto& order : test::CaptureSink::of_kind(run.records.commands, "OrderArrival")) {
    const auto actor = order.at("actor_id");
    const auto time = time_of(order);
    if ((actor != 5 && actor != 6) || (time >= 37 * kSecond && time < 43 * kSecond) ||
        (time >= 77 * kSecond && time < 83 * kSecond)) {
      continue;
    }
    auto& window = windows.at((time >= 40 * kSecond ? 1U : 0U) + (time >= 80 * kSecond ? 1U : 0U));
    ++window.orders;
    window.buys += order.at("payload").at("side") == "Buy" ? 1U : 0U;
    window.market += order.at("payload").at("type") == "Market" ? 1U : 0U;
  }
  const auto& before = windows.at(0);
  const auto& during = windows.at(1);
  const auto& after = windows.at(2);
  // Every order inside is a market order and nearly every one buys.
  EXPECT_EQ(during.market, during.orders);
  EXPECT_GT(during.buys * 100, during.orders * 85);
  EXPECT_LT(before.buys * 100, before.orders * 75);
  EXPECT_LT(after.buys * 100, after.orders * 75);
  EXPECT_LT(after.market, after.orders);
  // Three times the rate over a window of 34 s against 37 s before it.
  EXPECT_GT(during.orders, 2 * before.orders);
  EXPECT_GT(during.orders, 2 * after.orders);
  // Only the named agents change pace; the value trader wakes exactly as without it.
  auto calm = config;
  test::scenario(calm).shocks.clear();
  EXPECT_EQ(test::scenario(run.summary).agents.at(2).wakeups,
            test::scenario(test::capture(calm).summary).agents.at(2).wakeups);
}

TEST(ScenarioRunTest, LatencyDegradationDelaysOrdersInsideItsWindowAndKeepsThemInOrder) {
  auto config = test::scenario_fixture();
  auto& noise = test::scenario(config).agents.at(4); // agent 5: one wakeup a second
  noise.wakeup.distribution = WakeupDistribution::Fixed;
  test::scenario(config).shocks = {
      LatencyDegradation{
          .start = SimTime{20 * kSecond},
          .end = SimTime{25 * kSecond},
          .agents = {AgentId{5}},
          .added_order_entry = SimTime{2'500 * kMillisecond},
          .added_report = SimTime{10 * kMillisecond},
          .added_market_data = SimTime{10 * kMillisecond},
      },
  };
  const auto run = test::capture(config);

  // Wakeups at 20 s to 24 s send at 2.5 s plus the configured 1 ms. Those at 25 s and
  // 26 s are back to 1 ms but wait behind the message sent at 24 s, due at 26.501 s.
  std::uint64_t previous = 0;
  std::uint64_t delayed = 0;
  for (const auto& line : run.records.commands) {
    const auto record = Json::parse(line);
    if (record.at("kind") == "Header" || record.at("actor_id") != 5) {
      continue;
    }
    const auto time = time_of(record);
    EXPECT_GE(time, previous); // first in, first out on the order path
    previous = time;
    const bool held = time > 20 * kSecond && time < 27 * kSecond;
    EXPECT_EQ(time % kSecond, held ? 501 * kMillisecond : kMillisecond) << time;
    delayed += held ? 1U : 0U;
  }
  EXPECT_GE(delayed, 5U);
}

TEST(ScenarioRunTest, QuoteWithdrawalStressCancelsEveryWorkingQuoteAtOnceAndMakersRequote) {
  auto config = test::scenario_fixture();
  const auto calm = test::capture(config);
  test::scenario(config).shocks = {
      QuoteWithdrawalStress{.at = SimTime{30 * kSecond}, .agents = {AgentId{1}, AgentId{2}}},
  };
  const auto run = test::capture(config);
  const auto shocks = test::CaptureSink::of_kind(run.records.trace, "ScenarioShock");
  ASSERT_EQ(shocks.size(), 1U);
  EXPECT_EQ(shocks.at(0).at("payload").at("phase"), "Instant");
  EXPECT_EQ(test::scenario(run.summary).shock_transitions, 1U);

  // Each maker works two levels a side. Its cancels arrive one order-entry latency after
  // the shock, which no maker wakeup (every 500 ms from 0) coincides with.
  const auto burst = [](const test::CapturedRun& captured) {
    std::uint64_t count = 0;
    for (const auto& cancel :
         test::CaptureSink::of_kind(captured.records.commands, "CancelArrival")) {
      const auto actor = cancel.at("actor_id");
      count +=
          (actor == 1 || actor == 2) && time_of(cancel) == (30 * kSecond) + kMillisecond ? 1U : 0U;
    }
    return count;
  };
  EXPECT_EQ(burst(calm), 0U);
  EXPECT_GE(burst(run), 6U);
  EXPECT_LE(burst(run), 8U);

  // The makers requote at their next wakeup, so the book is two-sided again a second later.
  for (const auto& snapshot : test::CaptureSink::of_kind(run.records.trace, "BookSnapshot")) {
    if (time_of(snapshot) == 31 * kSecond) {
      EXPECT_FALSE(snapshot.at("payload").at("bids").empty());
      EXPECT_FALSE(snapshot.at("payload").at("asks").empty());
    }
  }
}

TEST(ScenarioRunTest, RunsWithOrderFlowLatencyAndQuoteStressShocksReplayExactly) {
  const test::TempDirectory directory;
  auto config = test::scenario_fixture();
  test::scenario(config).shocks = {
      OrderFlowImbalance{
          .start = SimTime{10 * kSecond},
          .end = SimTime{30 * kSecond},
          .agents = {AgentId{6}},
          .buy_probability_ppm = 100'000,
          .market_order_ppm = 800'000,
          .arrival_rate_ppm = 2 * kPpm,
      },
      LatencyDegradation{
          .start = SimTime{15 * kSecond},
          .end = SimTime{45 * kSecond},
          .agents = {AgentId{1}, AgentId{3}},
          .added_order_entry = SimTime{20 * kMillisecond},
          .added_report = SimTime{5 * kMillisecond},
          .added_market_data = SimTime{50 * kMillisecond},
      },
      QuoteWithdrawalStress{.at = SimTime{20 * kSecond}, .agents = {AgentId{2}, AgentId{1}}},
  };
  const auto summary = Simulation{config, directory.run()}.run();
  EXPECT_EQ(test::scenario(summary).shock_transitions, 5U);
  const auto replay = verify_replay(directory.run());
  ASSERT_TRUE(replay.verified) << replay_result_json(replay);
  EXPECT_EQ(replay.summary, summary);
}

TEST(ScenarioRunTest, ReplayRegeneratesEveryAgentDecisionAndFlagsTheFirstDivergence) {
  const test::TempDirectory directory;
  auto config = test::scenario_fixture();
  test::scenario(config).shocks = {
      FundamentalJump{.at = SimTime{30 * kSecond}, .delta_milliticks = 40'000},
  };
  const auto summary = Simulation{config, directory.run()}.run();
  auto replay = verify_replay(directory.run());
  ASSERT_TRUE(replay.verified) << replay_result_json(replay);
  EXPECT_EQ(replay.summary, summary);
  EXPECT_EQ(Simulation{config}.run(), summary);

  // Changing one agent order in the command log is caught before it reaches the exchange.
  const auto path = directory.run() / "commands.jsonl";
  const auto original = test::read(path);
  const auto first_order = original.find(R"("kind":"OrderArrival")");
  ASSERT_NE(first_order, std::string::npos);
  const auto line_start = original.rfind('\n', first_order) + 1;
  const auto quantity = original.find(R"("quantity":)", first_order);
  ASSERT_NE(quantity, std::string::npos);
  auto tampered = original;
  tampered.insert(quantity + 11, "9");
  test::write(path, tampered);
  replay = verify_replay(directory.run());
  EXPECT_FALSE(replay.verified);
  EXPECT_EQ(replay.code, "TraceMismatch");
  EXPECT_EQ(replay.sequence.value(),
            Json::parse(original.substr(line_start, original.find('\n', line_start) - line_start))
                .at("sequence")
                .get<std::uint64_t>());

  // A different seed regenerates different decisions, so the stored logs cannot verify.
  test::write(path, original);
  auto reseeded = config;
  reseeded.seed += 1;
  test::write(directory.run() / "config.json", canonical_config(reseeded) + '\n');
  EXPECT_FALSE(verify_replay(directory.run()).verified);
}

// Phase 2A exit gate: one simulated hour with meaningful trading, a two-sided book
// throughout, every policy active, and activity pinned to a versioned golden summary.
TEST(ScenarioRunTest, BaselineHourIsATwoSidedActiveMarketAndMatchesItsGoldenSummary) {
  const std::filesystem::path source{MARKETTWIN_SOURCE_DIR};
  const auto config = load_config(source / "configs/baseline.json");
  ASSERT_EQ(config.limits.horizon, SimTime{3'600 * kSecond});
  const auto run = test::capture(config);
  const auto& scenario = test::scenario(run.summary);
  EXPECT_EQ(run.summary.stop_reason, StopReason::Horizon);
  EXPECT_GT(run.summary.trade_count, 5'000U);

  const auto snapshots = test::CaptureSink::of_kind(run.records.trace, "BookSnapshot");
  ASSERT_EQ(snapshots.size(), 3'600U);
  std::int64_t widest = 0;
  for (const auto& snapshot : snapshots) {
    const auto& book = snapshot.at("payload");
    ASSERT_FALSE(book.at("best_bid_ticks").is_null()) << time_of(snapshot);
    ASSERT_FALSE(book.at("best_ask_ticks").is_null()) << time_of(snapshot);
    widest = std::max(widest, book.at("best_ask_ticks").get<std::int64_t>() -
                                  book.at("best_bid_ticks").get<std::int64_t>());
  }
  EXPECT_LE(widest, 10);

  std::set<std::string> policies;
  std::int64_t net = 0;
  for (const auto& agent : scenario.agents) {
    EXPECT_GT(agent.fills, 0U) << agent.id.value();
    EXPECT_EQ(agent.intents_dropped, 0U) << agent.id.value();
    policies.insert(agent.policy);
    net += agent.net_position;
  }
  EXPECT_EQ(policies.size(), 5U);
  EXPECT_EQ(net, 0);

  // The market price is endogenous but stays tied to the latent value by value traders.
  const auto last = snapshots.back().at("payload");
  const auto mid_milliticks = (last.at("best_bid_ticks").get<std::int64_t>() +
                               last.at("best_ask_ticks").get<std::int64_t>()) *
                              (kMilliticks / 2);
  EXPECT_LT(std::abs(mid_milliticks - scenario.fundamental_milliticks), 30 * kMilliticks);

  const auto expected =
      Json::parse(test::read(source / "core/tests/replay/golden/phase2a_baseline_summary.json"));
  EXPECT_EQ(Json::parse(summary_json(run.summary)), expected);

  // Phase 2B: the same hour's books and market statistics, pinned alongside it.
  const auto metrics =
      Json::parse(test::read(source / "core/tests/replay/golden/phase2b_baseline_metrics.json"));
  EXPECT_EQ(Json::parse(metrics_json(config, run.summary)), metrics);
  EXPECT_EQ(run.summary.report.market.two_sided_samples, 3'600U);
  EXPECT_EQ(run.summary.report.samples_by_mark.at(static_cast<std::size_t>(MarkSource::Midpoint)),
            3'600U);
}

TEST(ScenarioRunTest, ShippedShockScenariosLoadRunAndApplyTheirShocks) {
  const std::filesystem::path source{MARKETTWIN_SOURCE_DIR};
  const std::map<std::string, std::uint64_t> transitions{
      {"fundamental_shock", 1}, {"liquidity_shock", 1}, {"volatility_shock", 1},
      {"order_flow_shock", 1},  {"latency_shock", 1},   {"quote_stress_shock", 1},
  };
  for (const auto& [name, expected] : transitions) {
    auto config = load_config(source / "configs" / (name + ".json"));
    ASSERT_EQ(test::scenario(config).shocks.size(), 1U) << name;
    // Run only until just after the shock starts; the full hour is covered by the baseline.
    const auto start = std::visit(
        [](const auto& shock) {
          if constexpr (requires { shock.at; }) {
            return shock.at;
          } else {
            return shock.start;
          }
        },
        test::scenario(config).shocks.front());
    config.limits.horizon = SimTime{start.value() + kSecond};
    const auto summary = Simulation{config}.run();
    EXPECT_EQ(test::scenario(summary).shock_transitions, expected) << name;
    EXPECT_GT(summary.trade_count, 1'000U) << name;
  }
}

TEST(ScenarioRunTest, ScriptedCommandsAndStopsComposeWithAgents) {
  auto config = test::scenario_fixture();
  config.commands = {test::at(10 * kSecond, StopCommand{})};
  const auto run = test::capture(config);
  EXPECT_EQ(run.summary.stop_reason, StopReason::Requested);
  EXPECT_EQ(run.summary.final_time, SimTime{10 * kSecond});
  EXPECT_GT(run.summary.trade_count, 0U);
}

} // namespace
} // namespace markettwin
