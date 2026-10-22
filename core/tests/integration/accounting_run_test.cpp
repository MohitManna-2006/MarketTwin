#include "markettwin/common/hash.hpp"
#include "markettwin/telemetry/replay.hpp"

#include "../test_support.hpp"

#include <gtest/gtest.h>
#include <numeric>

namespace markettwin {
namespace {
using Json = nlohmann::json;
using test::kSecond;

std::vector<Json> parsed(const std::vector<std::string>& lines) {
  std::vector<Json> records;
  records.reserve(lines.size());
  for (const auto& line : lines) {
    records.push_back(Json::parse(line));
  }
  return records;
}

std::vector<Json> of_kind(const std::vector<Json>& records, std::string_view kind) {
  std::vector<Json> result;
  for (const auto& record : records) {
    if (record.at("kind") == kind) {
      result.push_back(record);
    }
  }
  return result;
}

const AgentReport& agent(const RunSummary& summary, std::uint64_t id) {
  for (const auto& item : summary.report.agents) {
    if (item.account.agent == AgentId{id}) {
      return item;
    }
  }
  throw std::logic_error("no such agent in the report");
}

Valuation valuation(const AgentReport& report) {
  if (!report.valuation) {
    throw std::logic_error("agent has no valuation");
  }
  return *report.valuation;
}

RunConfig with_fees(RunConfig config) {
  config.accounting.fees = {.maker_ppm = -100, .taker_ppm = 300};
  config.accounting.default_initial_cash = Money{1'000'000'000};
  return config;
}

std::string code_of(const std::function<void()>& action) {
  try {
    action();
  } catch (const RunError& error) {
    return error.code();
  }
  return "no error";
}

// The scripted fixture: agent 1 offers 5 at 100, agent 2 buys 3 of them, agent 1 cancels
// the remaining 2, and a depth snapshot is taken at t = 20.
TEST(AccountingRunTest, AScriptedRunPostsItsExecutionAndReportsExactBalances) {
  auto config = test::fixture();
  config.tick_size_minor = 25;
  const auto run = test::capture(config);
  const auto& seller = agent(run.summary, 1);
  const auto& buyer = agent(run.summary, 2);
  EXPECT_EQ(buyer.account.cash, Money{-7'500}); // 100 ticks * 3 shares * 25 minor units
  EXPECT_EQ(buyer.account.position, 3);
  EXPECT_EQ(buyer.account.cost_basis, Money{7'500});
  EXPECT_EQ(seller.account.cash, Money{7'500});
  EXPECT_EQ(seller.account.position, -3);
  EXPECT_TRUE(seller.policy.empty());

  // The book is empty at the end, so the last trade is the mark, and says so.
  const auto& report = run.summary.report;
  EXPECT_EQ(report.final_mark, (Mark{MarkSource::LastTrade, 200}));
  EXPECT_EQ(report.samples_by_mark.at(static_cast<std::size_t>(MarkSource::LastTrade)), 1U);
  EXPECT_EQ(valuation(buyer).equity, Money{0});
  EXPECT_EQ(valuation(buyer).unrealized_pnl, Money{0});
  EXPECT_EQ(buyer.equity_observations, 3U); // start, the snapshot, and the end
  EXPECT_EQ(buyer.max_drawdown, Money{0});
  EXPECT_EQ(buyer.mean_abs_position_millishares, 3'000);

  const auto& market = report.market;
  EXPECT_EQ(market.trade_count, 1U);
  EXPECT_EQ(market.volume, Quantity{3});
  EXPECT_EQ(market.vwap_milliticks, 100'000);
  EXPECT_EQ(report.traded_notional, Money{7'500});
  EXPECT_EQ(market.orders_accepted, 2U);
  EXPECT_EQ(market.cancels_requested, 2U); // one by the wrong owner
  EXPECT_EQ(market.cancels_succeeded, 1U);
  EXPECT_EQ(market.canceled_quantity, Quantity{2});
  EXPECT_EQ(market.ask_only_ns, 3U);
  EXPECT_EQ(market.empty_ns, 17U);
  EXPECT_EQ(market.total_ns, 20U);
  EXPECT_FALSE(market.mean_spread_milliticks.has_value());
  EXPECT_EQ(seller.flow.maker_fills, 1U);
  EXPECT_EQ(seller.flow.maker_fills_measured, 0U); // the buyer met a one-sided book
  EXPECT_EQ(buyer.fill_rate_ppm, 1'000'000U);
  EXPECT_EQ(seller.fill_rate_ppm, 600'000U);

  // Header, one posting, one sample, and the terminal record, each canonical JSON.
  ASSERT_EQ(run.records.accounting.size(), 4U);
  const auto records = parsed(run.records.accounting);
  for (std::size_t index = 0; index < records.size(); ++index) {
    EXPECT_EQ(records.at(index).dump() + '\n', run.records.accounting.at(index));
  }
  EXPECT_EQ(records.at(0).at("kind"), "Header");
  EXPECT_EQ(records.at(0).at("schema_version"), kAccountingSchema);
  EXPECT_EQ(records.at(0).at("units").at("tick_size_minor"), 25);
  EXPECT_EQ(records.at(1).at("kind"), "LedgerEntry");
  EXPECT_EQ(records.at(1).at("sequence"), 1);
  EXPECT_EQ(records.at(1).at("timestamp_ns"), 1);
  EXPECT_EQ(records.at(1).at("payload").at("buyer").at("cash_minor"), -7'500);
  EXPECT_EQ(records.at(1).at("payload").at("seller").at("position"), -3);
  EXPECT_EQ(records.at(2).at("kind"), "Sample");
  EXPECT_EQ(records.at(2).at("payload").at("mark_source"), "LastTrade");
  EXPECT_TRUE(records.at(2).at("payload").at("fundamental_milliticks").is_null());
  EXPECT_TRUE(records.at(2).at("payload").at("spread_ticks").is_null());
  EXPECT_EQ(records.at(3).at("kind"), "AccountingCompleted");
  // The stream ends by committing to the metrics document published beside it.
  EXPECT_EQ(records.at(3).at("payload").at("metrics_digest"), run.summary.metrics_digest);
  EXPECT_EQ(run.summary.metrics_digest, sha256(metrics_json(config, run.summary) + '\n'));
  EXPECT_EQ(records.at(3).at("payload").at("trade_count"), 1);
}

TEST(AccountingRunTest, ARunWithNoTradesHasBalancesButNoMarkOrPriceStatistics) {
  RunConfig config;
  config.run_id = "quiet";
  config.commands = {test::at(5, test::order(1, Side::Sell)), test::at(9, SnapshotCommand{})};
  const auto summary = Simulation{config}.run();
  EXPECT_EQ(summary.report.final_mark, Mark{});
  EXPECT_EQ(summary.report.samples_by_mark.at(static_cast<std::size_t>(MarkSource::None)), 1U);
  EXPECT_FALSE(summary.report.market.vwap_milliticks.has_value());
  EXPECT_FALSE(summary.report.market.last_trade_price.has_value());
  EXPECT_EQ(summary.report.market.fill_rate_ppm, 0U);
  // Nobody holds a position, so equity is still defined: it is cash.
  EXPECT_EQ(valuation(agent(summary, 1)).equity, Money{0});
  const auto document = Json::parse(metrics_json(config, summary));
  EXPECT_TRUE(document.at("final_mark").at("twice_price_ticks").is_null());
  EXPECT_TRUE(document.at("market").at("vwap_milliticks").is_null());
  EXPECT_EQ(document.at("agents").at(0).at("resting_quantity"), 5);
}

TEST(AccountingRunTest, AScenarioRunConservesCashSharesAndOrderFlow) {
  const auto config = with_fees(test::scenario_fixture());
  const auto run = test::capture(config);
  const auto& report = run.summary.report;
  ASSERT_GT(run.summary.trade_count, 100U);

  EXPECT_EQ(report.totals.position, 0);
  EXPECT_EQ(report.totals.initial_cash, Money{6'000'000'000});
  EXPECT_EQ(checked_add(report.totals.cash, report.totals.fee_sink), report.totals.initial_cash);
  EXPECT_GT(report.totals.fee_sink, Money{0});

  std::int64_t total_pnl = 0;
  std::int64_t gross = 0;
  Int128 maker_edge = 0;
  Int128 taker_cost = 0;
  const auto& activity = test::scenario(run.summary).agents;
  ASSERT_EQ(report.agents.size(), activity.size());
  for (std::size_t index = 0; index < report.agents.size(); ++index) {
    const auto& item = report.agents.at(index);
    const auto& account = item.account;
    const auto valued = valuation(item);
    EXPECT_EQ(valued.total_pnl,
              checked_add(checked_subtract(account.realized_pnl, account.fees_paid),
                          valued.unrealized_pnl));
    EXPECT_EQ(valued.equity, checked_add(account.initial_cash, valued.total_pnl));
    total_pnl += valued.total_pnl.value();
    gross += account.gross_notional.value();
    maker_edge += item.flow.maker_edge_half_tick_shares;
    taker_cost += item.flow.taker_cost_half_tick_shares;
    // Every accepted share was filled, canceled, expired, or is still resting.
    const auto& flow = item.flow;
    EXPECT_EQ(flow.maker_quantity.value() + flow.taker_quantity.value() +
                  flow.canceled_quantity.value() + flow.expired_quantity.value() +
                  item.resting_quantity.value(),
              flow.submitted_quantity.value())
        << account.agent.value();
    // The ledger agrees with the Phase 2A share counts taken at the exchange.
    EXPECT_EQ(account.agent, activity.at(index).id);
    EXPECT_EQ(account.position, activity.at(index).net_position);
    EXPECT_EQ(account.fills, activity.at(index).fills);
    EXPECT_EQ(checked_add(account.bought, account.sold), activity.at(index).filled_quantity);
    EXPECT_EQ(item.policy, activity.at(index).policy);
    EXPECT_GE(item.max_drawdown, Money{0});
    EXPECT_EQ(item.equity_observations, 62U); // start, sixty samples, end
  }
  // Trading is zero-sum before fees. Each account's mark may round by half a minor unit.
  EXPECT_LE(std::abs(total_pnl + report.totals.fee_sink.value()), 3);
  EXPECT_EQ(gross, 2 * report.traded_notional.value());
  EXPECT_EQ(maker_edge, taker_cost);
  EXPECT_EQ(report.market.filled_quantity.value(), 2 * report.market.volume.value());
  EXPECT_EQ(report.market.trade_count, run.summary.trade_count);
  EXPECT_EQ(report.market.total_ns, run.summary.final_time.value());
  EXPECT_EQ(report.market.samples, 60U);

  const auto records = parsed(run.records.accounting);
  EXPECT_EQ(of_kind(records, "LedgerEntry").size(), run.summary.trade_count);
  EXPECT_EQ(of_kind(records, "Sample").size(), 60U);
  for (std::size_t index = 1; index < records.size(); ++index) {
    EXPECT_EQ(records.at(index).at("sequence"), index); // one gap-free stream
  }
}

TEST(AccountingRunTest, FeesAndOpeningCashAreBookkeepingAndLeaveTheMarketUntouched) {
  const auto plain = Simulation{test::scenario_fixture()}.run();
  const auto funded = Simulation{with_fees(test::scenario_fixture())}.run();
  EXPECT_EQ(funded.trade_digest, plain.trade_digest);
  EXPECT_EQ(funded.book_digest, plain.book_digest);
  EXPECT_EQ(funded.event_count, plain.event_count);
  EXPECT_EQ(test::scenario(funded), test::scenario(plain));
  EXPECT_EQ(funded.report.market, plain.report.market);
  EXPECT_NE(funded.config_digest, plain.config_digest);
  EXPECT_NE(funded.accounting_digest, plain.accounting_digest);
  // Gross P&L is the same trade for trade; only cash and fees differ.
  for (std::size_t index = 0; index < plain.report.agents.size(); ++index) {
    const auto& before = plain.report.agents.at(index).account;
    const auto& after = funded.report.agents.at(index).account;
    EXPECT_EQ(after.realized_pnl, before.realized_pnl);
    EXPECT_EQ(after.cost_basis, before.cost_basis);
    EXPECT_EQ(checked_subtract(after.cash, after.initial_cash),
              checked_subtract(before.cash, after.fees_paid));
  }
}

TEST(AccountingRunTest, TheAccountingStreamIsDeterministicAndItsSeriesReproducesTheReport) {
  const auto config = with_fees(test::scenario_fixture());
  const auto run = test::capture(config);
  const auto again = test::capture(config);
  EXPECT_EQ(run.records.accounting, again.records.accounting);
  EXPECT_EQ(run.summary, again.summary);
  const auto bytes =
      std::accumulate(run.records.accounting.begin(), run.records.accounting.end(), std::string{});
  EXPECT_EQ(sha256(bytes), run.summary.accounting_digest);

  // Rebuild each agent's drawdown and inventory from nothing but the published series.
  const auto records = parsed(run.records.accounting);
  const auto samples = of_kind(records, "Sample");
  const auto& ids = records.front().at("agent_ids");
  ASSERT_EQ(records.front().at("agent_columns").at(1), "position");
  ASSERT_EQ(records.front().at("agent_columns").at(5), "equity_minor");
  for (std::size_t index = 0; index < ids.size(); ++index) {
    const auto& item = agent(run.summary, ids.at(index).get<std::uint64_t>());
    EquityTracker tracker;
    tracker.observe(item.account.initial_cash);
    std::int64_t inventory = 0;
    for (const auto& sample : samples) {
      const auto& row = sample.at("payload").at("agents").at(index);
      tracker.observe(Money{row.at(5).get<std::int64_t>()});
      inventory += std::abs(row.at(1).get<std::int64_t>());
    }
    tracker.observe(valuation(item).equity);
    EXPECT_EQ(tracker.peak(), item.peak_equity);
    EXPECT_EQ(tracker.max_drawdown(), item.max_drawdown);
    EXPECT_EQ(narrow(divide_round(Int128{inventory} * 1'000, 60)),
              item.mean_abs_position_millishares);
  }
  // The fundamental is published for research, as its own column, and is not the mark.
  const auto& last = samples.back().at("payload");
  EXPECT_FALSE(last.at("fundamental_milliticks").is_null());
  EXPECT_EQ(last.at("mark_source"), "Midpoint");
  EXPECT_NE(last.at("mark_twice_ticks").get<std::int64_t>() * 500,
            last.at("fundamental_milliticks").get<std::int64_t>());
  EXPECT_EQ(last.at("trade_count"), run.summary.trade_count);
}

TEST(AccountingRunTest, ACreditLimitKeepsALimitOrderAgentsCashAboveItsFloor) {
  // Agent 2 quotes two levels of 100 shares near 15,000 ticks but can fund only 100.
  auto constrained = test::scenario_fixture();
  constrained.accounting.accounts.push_back(
      {.agent = AgentId{2}, .initial_cash = Money{1'600'000}, .credit_limit = Money{0}});
  auto unconstrained = constrained;
  unconstrained.accounting.accounts.front().credit_limit.reset();

  const auto free = Simulation{unconstrained}.run();
  const auto bound = Simulation{constrained}.run();
  // Left alone, the maker runs a cash deficit; that is what the limit exists to stop.
  EXPECT_LT(agent(free, 2).account.min_cash, Money{0});
  EXPECT_EQ(agent(free, 2).cash_refusals, 0U);
  EXPECT_FALSE(agent(free, 2).credit_limit.has_value());

  const auto& maker = agent(bound, 2);
  EXPECT_EQ(maker.credit_limit, Money{0});
  EXPECT_GT(maker.cash_refusals, 0U);
  EXPECT_GE(maker.account.min_cash, Money{0});
  EXPECT_GT(maker.account.bought, Quantity{0}); // constrained, not switched off
  EXPECT_GT(maker.account.sold, Quantity{0});
  // Refusals are a subset of the intents the environment dropped.
  EXPECT_GE(test::scenario(bound).agents.at(1).intents_dropped, maker.cash_refusals);
  // The constraint changes what the maker does, so the market differs, and no other
  // agent is refused anything.
  EXPECT_NE(bound.trade_digest, free.trade_digest);
  for (const std::uint64_t id : {1U, 3U, 4U, 5U, 6U}) {
    EXPECT_EQ(agent(bound, id).cash_refusals, 0U);
  }
  EXPECT_EQ(bound.report.totals.position, 0);

  // A credit line larger than anything it could spend restores the unconstrained run.
  auto financed = constrained;
  financed.accounting.accounts.front().credit_limit = Money{1'000'000'000};
  const auto roomy = Simulation{financed}.run();
  EXPECT_EQ(agent(roomy, 2).cash_refusals, 0U);
  EXPECT_EQ(roomy.trade_digest, free.trade_digest);
}

TEST(AccountingRunTest, AccountingConfigIsSchemaThreeAndDefaultsKeepEarlierBytes) {
  const auto plain = test::scenario_fixture();
  const auto base = Json::parse(canonical_config(plain));
  EXPECT_EQ(base.at("schema_version"), 2);
  EXPECT_FALSE(base.contains("accounting"));

  auto funded = with_fees(plain);
  funded.accounting.accounts = {
      {.agent = AgentId{3}, .initial_cash = Money{-5}, .credit_limit = Money{7}},
      {.agent = AgentId{1}, .initial_cash = Money{9}, .credit_limit = {}},
  };
  const auto bytes = canonical_config(funded);
  const auto value = Json::parse(bytes);
  EXPECT_EQ(value.at("schema_version"), 3);
  EXPECT_EQ(value.at("accounting").at("fees").at("maker_ppm"), -100);
  EXPECT_EQ(value.at("accounting").at("accounts").at(0).at("agent_id"), 1); // sorted by agent
  EXPECT_TRUE(value.at("accounting").at("accounts").at(0).at("credit_limit_minor").is_null());
  EXPECT_EQ(value.at("accounting").at("accounts").at(1).at("credit_limit_minor"), 7);
  const auto reparsed = parse_config(bytes);
  EXPECT_EQ(canonical_config(reparsed), bytes);
  EXPECT_EQ(reparsed.accounting.default_initial_cash, Money{1'000'000'000});

  // A scripted run can carry accounting too: schema 3 with a null scenario.
  auto scripted = test::fixture();
  scripted.accounting.fees.taker_ppm = 10;
  const auto scripted_value = Json::parse(canonical_config(scripted));
  EXPECT_EQ(scripted_value.at("schema_version"), 3);
  EXPECT_TRUE(scripted_value.at("scenario").is_null());
  EXPECT_FALSE(parse_config(scripted_value.dump()).scenario.has_value());

  // Schema 3 spelling of default accounting is accepted and canonicalizes back to 2.
  auto spelled = base;
  spelled.at("schema_version") = 3;
  spelled.emplace("accounting", Json::parse(canonical_config(scripted)).at("accounting"));
  spelled.at("accounting").at("fees").at("taker_ppm") = 0;
  EXPECT_EQ(canonical_config(parse_config(spelled.dump())), base.dump());
}

TEST(AccountingRunTest, RejectsMalformedAndOutOfBoundsAccounting) {
  const auto valid = Json::parse(canonical_config(with_fees(test::scenario_fixture())));
  const auto rejected = [&](const std::function<void(Json&)>& mutate) {
    auto changed = valid;
    mutate(changed);
    return code_of([&] { static_cast<void>(parse_config(changed.dump())); });
  };
  const Json account{{"agent_id", 1}, {"initial_cash_minor", 0}, {"credit_limit_minor", 0}};
  EXPECT_EQ(rejected([](Json&) {}), "no error");
  EXPECT_EQ(rejected([](Json& value) { value.erase("accounting"); }), "InvalidConfig");
  EXPECT_EQ(rejected([](Json& value) { value.at("accounting").emplace("extra", 1); }),
            "InvalidConfig");
  EXPECT_EQ(rejected([](Json& value) { value.at("accounting").at("fees").erase("maker_ppm"); }),
            "InvalidConfig");
  EXPECT_EQ(rejected([](Json& value) { value.at("accounting").at("fees").at("taker_ppm") = 0.5; }),
            "InvalidConfig");
  EXPECT_EQ(
      rejected([](Json& value) { value.at("accounting").at("fees").at("taker_ppm") = 1'000'001; }),
      "InvalidConfig");
  EXPECT_EQ(
      rejected([](Json& value) { value.at("accounting").at("fees").at("maker_ppm") = -1'000'001; }),
      "InvalidConfig");
  EXPECT_EQ(rejected([](Json& value) {
              value.at("accounting").at("default_initial_cash_minor") = 2'000'000'000'000'000'000;
            }),
            "InvalidConfig");
  EXPECT_EQ(
      rejected([&](Json& value) { value.at("accounting").at("accounts") = {account, account}; }),
      "InvalidConfig"); // one agent twice
  EXPECT_EQ(rejected([&](Json& value) {
              auto unknown = account;
              unknown.at("agent_id") = 99;
              value.at("accounting").at("accounts") = {unknown};
            }),
            "InvalidConfig");
  EXPECT_EQ(rejected([&](Json& value) {
              auto negative = account;
              negative.at("credit_limit_minor") = -1;
              value.at("accounting").at("accounts") = {negative};
            }),
            "InvalidConfig");
  EXPECT_EQ(rejected([&](Json& value) { value.at("accounting").at("accounts") = {account}; }),
            "no error");
  EXPECT_EQ(rejected([](Json& value) { value.at("schema_version") = 5; }), "UnsupportedSchema");

  // A credit limit needs an enforcement point, which only a scenario agent has.
  auto scripted = test::fixture();
  scripted.accounting.accounts.push_back(
      {.agent = AgentId{1}, .initial_cash = {}, .credit_limit = Money{0}});
  EXPECT_EQ(code_of([&] { validate_config(scripted); }), "InvalidConfig");
  scripted.accounting.accounts.front().credit_limit.reset();
  EXPECT_EQ(code_of([&] { validate_config(scripted); }), "no error");
}

TEST(AccountingRunTest, TheShippedAccountingScenarioLoadsRunsAndReconciles) {
  const std::filesystem::path source{MARKETTWIN_SOURCE_DIR};
  auto config = load_config(source / "configs/baseline_accounting.json");
  EXPECT_EQ(config.accounting.fees, (FeeSchedule{.maker_ppm = -20, .taker_ppm = 30}));
  config.limits.horizon = SimTime{120 * kSecond};
  const auto summary = Simulation{config}.run();
  EXPECT_GT(summary.trade_count, 100U);
  const auto& totals = summary.report.totals;
  EXPECT_EQ(checked_add(totals.cash, totals.fee_sink), totals.initial_cash);
  EXPECT_GT(totals.fee_sink, Money{0}); // takers pay more than makers are rebated
  std::int64_t rebates = 0;
  for (const std::uint64_t maker : {1U, 2U, 3U}) {
    EXPECT_EQ(agent(summary, maker).credit_limit, Money{0});
    EXPECT_GE(agent(summary, maker).account.min_cash, Money{0});
    EXPECT_LE(agent(summary, maker).account.fees_paid, Money{0}); // makers only ever rest
    rebates += agent(summary, maker).account.fees_paid.value();
  }
  EXPECT_LT(rebates, 0);
}

TEST(AccountingRunTest, ReplayVerifiesTheAccountingStreamAndPinpointsADivergentPosting) {
  const test::TempDirectory directory;
  const auto config = with_fees(test::scenario_fixture());
  Simulation simulation{config, directory.run()};
  const auto summary = simulation.run();
  const auto paths = simulation.artifacts();
  EXPECT_EQ(summary.accounting_digest, sha256(test::read(paths.accounting)));
  EXPECT_EQ(test::read(paths.metrics), metrics_json(config, summary) + '\n');
  const auto manifest = Json::parse(test::read(paths.manifest));
  EXPECT_EQ(manifest.at("schema_version"), 2);
  EXPECT_EQ(manifest.at("artifacts").at("accounting.jsonl"), summary.accounting_digest);
  EXPECT_EQ(manifest.at("artifacts").at("metrics.json"), sha256(test::read(paths.metrics)));
  EXPECT_EQ(manifest.at("schemas"), (Json{{"trace", 1}, {"accounting", 1}, {"metrics", 1}}));
  const auto verified = verify_replay(directory.run());
  ASSERT_TRUE(verified.verified) << replay_result_json(verified);
  EXPECT_EQ(verified.summary, summary);

  // Change one booked cash balance in the tenth posting.
  const auto original = test::read(paths.accounting);
  std::size_t position = 0;
  for (int posting = 0; posting < 10; ++posting) {
    position = original.find(R"({"kind":"LedgerEntry")", position + 1);
    ASSERT_NE(position, std::string::npos);
  }
  const auto line_end = original.find('\n', position);
  const auto record = Json::parse(original.substr(position, line_end - position));
  const auto field = original.find("\"cash_minor\":", position) + 13;
  auto tampered = original;
  tampered.insert(field, tampered.at(field) == '-' ? "" : "1");
  if (tampered == original) {
    tampered.insert(field + 1, "1");
  }
  test::write(paths.accounting, tampered);
  const auto diverged = verify_replay(directory.run());
  EXPECT_FALSE(diverged.verified);
  EXPECT_EQ(diverged.code, "AccountingMismatch");
  EXPECT_EQ(diverged.sequence.value(), record.at("sequence").get<std::uint64_t>());
  EXPECT_NE(diverged.expected, diverged.actual);
  EXPECT_EQ(Json::parse(diverged.actual), record); // what the engine computes
  EXPECT_NE(diverged.previous_command.find("OrderArrival"), std::string::npos);

  test::write(paths.accounting, original + "{}\n");
  EXPECT_EQ(verify_replay(directory.run()).code, "ExtraRecords");
  test::write(paths.accounting, original.substr(0, original.size() - 1));
  EXPECT_EQ(verify_replay(directory.run()).code, "IncompleteTrace");
  auto header = original;
  header.replace(header.find("\"schema_version\":1"), 18, "\"schema_version\":2");
  test::write(paths.accounting, header);
  EXPECT_EQ(verify_replay(directory.run()).code, "UnsupportedSchema");
  test::write(paths.accounting, original);
  EXPECT_TRUE(verify_replay(directory.run()).verified);
}

TEST(AccountingRunTest, ReplayRejectsTamperedMetricsChecksumsSchemasAndMissingFiles) {
  const test::TempDirectory directory;
  Simulation simulation{test::fixture(), directory.run()};
  static_cast<void>(simulation.run());
  const auto paths = simulation.artifacts();

  const auto metrics = test::read(paths.metrics);
  auto changed = Json::parse(metrics);
  changed.at("agents").at(0).at("realized_pnl_minor") = 1;
  test::write(paths.metrics, changed.dump() + '\n');
  EXPECT_EQ(verify_replay(directory.run()).code, "MetricsMismatch");
  test::write(paths.metrics, metrics);

  const auto manifest = Json::parse(test::read(paths.manifest));
  auto edited = manifest;
  edited.at("artifacts").at("accounting.jsonl") = std::string(64, '0');
  test::write(paths.manifest, edited.dump());
  EXPECT_EQ(verify_replay(directory.run()).code, "ChecksumMismatch");
  edited = manifest;
  edited.at("artifacts").at("metrics.json") = std::string(64, '0');
  test::write(paths.manifest, edited.dump());
  EXPECT_EQ(verify_replay(directory.run()).code, "ChecksumMismatch");
  edited = manifest;
  edited.at("schemas").at("accounting") = 2;
  test::write(paths.manifest, edited.dump());
  EXPECT_EQ(verify_replay(directory.run()).code, "UnsupportedSchema");
  edited = manifest;
  edited.at("schema_version") = 1; // a Phase 2A manifest has no accounting to verify
  test::write(paths.manifest, edited.dump());
  EXPECT_EQ(verify_replay(directory.run()).code, "UnsupportedSchema");
  test::write(paths.manifest, manifest.dump());
  EXPECT_TRUE(verify_replay(directory.run()).verified);

  std::filesystem::remove(paths.metrics);
  EXPECT_EQ(verify_replay(directory.run()).code, "IoError");
  test::write(paths.metrics, metrics);
  std::filesystem::remove(paths.accounting);
  EXPECT_EQ(verify_replay(directory.run()).code, "IncompleteArtifacts");
}

TEST(AccountingRunTest, AnAccountingOverflowFailsTheRunExplicitlyAndPublishesNoManifest) {
  auto config = test::fixture();
  // 2^62 ticks * 3 shares overflows 64-bit money; the exchange itself is content.
  for (const auto index : {1U, 2U}) {
    std::get<Order>(config.commands.at(index).command).limit_price =
        PriceTicks{4'611'686'018'427'387'904};
  }
  EXPECT_EQ(code_of([&] { static_cast<void>(Simulation{config}.run()); }), "AccountingOverflow");
  const test::TempDirectory directory;
  EXPECT_THROW(static_cast<void>(Simulation{config, directory.run()}.run()), RunError);
  EXPECT_FALSE(std::filesystem::exists(directory.run() / "manifest.json"));
}

TEST(AccountingRunTest, TheAccountingStreamHasItsOwnByteBudget) {
  // Forty agents and forty snapshots: each sample row lists every agent, so the
  // accounting stream outgrows the trace and reaches the shared limit first.
  RunConfig config;
  config.run_id = "wide";
  config.exchange.agents.clear();
  for (std::uint64_t id = 1; id <= 40; ++id) {
    config.exchange.agents.emplace_back(id);
    config.commands.push_back(test::at(id, SnapshotCommand{.depth = 1}));
  }
  const auto run = test::capture(config);
  const auto size = [](const std::vector<std::string>& lines) {
    return std::accumulate(
        lines.begin(), lines.end(), std::size_t{0},
        [](std::size_t total, const std::string& line) { return total + line.size(); });
  };
  const auto trace = size(run.records.commands) + size(run.records.trace);
  const auto accounting = size(run.records.accounting);
  ASSERT_GT(accounting, trace);
  config.max_trace_bytes = accounting - 1;
  EXPECT_EQ(code_of([&] { static_cast<void>(Simulation{config}.run()); }), "ResourceLimit");
  config.max_trace_bytes = accounting;
  EXPECT_EQ(code_of([&] { static_cast<void>(Simulation{config}.run()); }), "no error");
}

} // namespace
} // namespace markettwin
