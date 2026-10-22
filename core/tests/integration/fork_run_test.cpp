#include "markettwin/common/hash.hpp"
#include "markettwin/experiment/experiment.hpp"
#include "markettwin/telemetry/fork.hpp"
#include "markettwin/telemetry/replay.hpp"

#include "../test_support.hpp"

#include <gtest/gtest.h>

namespace markettwin {
namespace {
using Json = nlohmann::json;
using test::kSecond;

std::string error_code(const RunConfig& config) {
  try {
    validate_config(config);
  } catch (const RunError& error) {
    return error.code();
  }
  return "no error";
}

// The scenario fixture with a latency shock on the makers from 20 s to 30 s.
RunConfig parent_config() {
  auto config = test::scenario_fixture();
  config.run_id = "parent";
  test::scenario(config).shocks = {
      LatencyDegradation{
          .start = SimTime{20 * kSecond},
          .end = SimTime{30 * kSecond},
          .agents = {AgentId{1}, AgentId{2}},
          .added_order_entry = SimTime{50'000'000},
          .added_report = SimTime{},
          .added_market_data = SimTime{},
      },
  };
  return config;
}

// The same scenario with agent 1 as an AI desk deciding every 10 s.
RunConfig desk_config() {
  auto config = parent_config();
  config.run_id = "desk";
  test::scenario(config).agents.at(0).policy = ModelDirectedParams{
      .initial =
          {
              .mode = DeskMode::Quote,
              .half_spread_ticks = 2,
              .skew_ticks = 0,
              .quote_size = Quantity{100},
              .levels = 2,
              .max_position = 300,
              .rationale = "start",
          },
      .decision_interval = SimTime{10 * kSecond},
      .max_half_spread_ticks = 10,
      .max_skew_ticks = 5,
      .mandate = "Make markets. Keep inventory within 300 shares.",
  };
  return config;
}

ForkStage stage_at(std::uint64_t seconds) {
  ForkStage stage;
  stage.at = SimTime{seconds * kSecond};
  return stage;
}

struct Family {
  test::TempDirectory directory;
  std::filesystem::path parent = directory.path() / "parent";

  explicit Family(const RunConfig& config, DecisionSource* decisions = nullptr) {
    Simulation simulation{config, parent};
    static_cast<void>(simulation.run(decisions));
  }
  // Runs a fork of the parent (or of `from`) and returns its directory.
  std::filesystem::path fork(const std::string& name, ForkStage stage,
                             DecisionSource* decisions = nullptr,
                             const std::filesystem::path& from = {}) const {
    const auto output = directory.path() / name;
    auto config = fork_of(from.empty() ? parent : from, std::move(stage), name);
    Simulation simulation{std::move(config), output};
    static_cast<void>(simulation.run(decisions));
    return output;
  }
};

Json report(const std::filesystem::path& parent, const std::filesystem::path& fork) {
  const auto result = verify_fork(parent, fork);
  EXPECT_TRUE(result.verified) << result.code << ": " << result.message;
  return result.verified ? Json::parse(result.report) : Json(nullptr);
}

std::vector<Json> decisions(const std::filesystem::path& run) {
  std::vector<Json> result;
  std::istringstream lines{test::read(run / "trace.jsonl")};
  std::string line;
  while (std::getline(lines, line)) {
    if (line.find(R"("kind":"Decision")") != std::string::npos) {
      result.push_back(Json::parse(line));
    }
  }
  return result;
}

TEST(ForkRunTest, AnEmptyForkSharesItsPrefixAndNeverDiverges) {
  const Family family{parent_config()};
  const auto fork = family.fork("empty", stage_at(15));
  const auto result = report(family.parent, fork);
  ASSERT_FALSE(result.is_null());
  EXPECT_GT(result.at("prefix").at("records").get<std::uint64_t>(), 100U);
  EXPECT_TRUE(result.at("divergence").at("first").is_null());
  EXPECT_TRUE(result.at("divergence").at("first_market").is_null());
  EXPECT_EQ(result.at("market").at("trades").at("difference"), 0);
  for (const auto& agent : result.at("agents")) {
    for (const auto& [name, value] : agent.at("values").items()) {
      EXPECT_EQ(value.at("difference"), 0) << name;
    }
  }
  // The fork's config is the parent's plus one stage, and it serializes as schema 4.
  const auto config = load_config(fork / "config.json");
  ASSERT_EQ(config.forks.size(), 1U);
  EXPECT_EQ(Json::parse(canonical_config(config)).at("schema_version"), 4);
  EXPECT_EQ(config.forks.front().parent_config_digest,
            sha256(canonical_config(load_config(family.parent / "config.json"))));
}

TEST(ForkRunTest, AnAddedShockDivergesAtTheForkAndARemovedOneNeverHappens) {
  const Family family{parent_config()};
  auto stress = stage_at(15);
  stress.add_shocks = {QuoteWithdrawalStress{.at = SimTime{15 * kSecond}, .agents = {AgentId{1}}}};
  const auto added = report(family.parent, family.fork("stress", stress));
  ASSERT_FALSE(added.is_null());
  const auto& first = added.at("divergence").at("first");
  ASSERT_FALSE(first.is_null());
  EXPECT_GE(first.at("timestamp_ns").get<std::uint64_t>(), 15 * kSecond);
  EXPECT_FALSE(added.at("divergence").at("first_market").is_null());

  auto quiet = stage_at(15);
  quiet.remove_shocks = {0};
  const auto fork = family.fork("quiet", quiet);
  const auto removed = report(family.parent, fork);
  ASSERT_FALSE(removed.is_null());
  const auto& gone = removed.at("divergence").at("first");
  ASSERT_FALSE(gone.is_null());
  EXPECT_EQ(gone.at("parent").at("kind"), "ScenarioShock");
  EXPECT_EQ(gone.at("timestamp_ns"), 20 * kSecond);
  EXPECT_EQ(test::read(fork / "trace.jsonl").find(R"("kind":"ScenarioShock")"), std::string::npos);
}

TEST(ForkRunTest, APolicySwitchKeepsTheAgentAndAForkOfAForkChainsItsStages) {
  const Family family{parent_config()};
  auto switched = stage_at(10);
  switched.policy_overrides = {
      {
          .agent = AgentId{1},
          .policy = MomentumParams{.short_window = 2, .long_window = 4},
      },
  };
  const auto first = family.fork("switched", switched);
  const auto result = report(family.parent, first);
  ASSERT_FALSE(result.is_null());
  EXPECT_EQ(result.at("agents").at(0).at("fork_policy"), "MarketMaker"); // the config's name
  // A momentum trader cannot be withdrawn as a maker: the later latency shock is fine,
  // a later withdrawal is not.
  auto withdrawn = switched;
  withdrawn.add_shocks = {
      LiquidityWithdrawal{
          .start = SimTime{40 * kSecond},
          .end = SimTime{50 * kSecond},
          .agents = {AgentId{1}},
          .quote_size_ppm = 0,
      },
  };
  EXPECT_THROW(static_cast<void>(fork_of(family.parent, withdrawn, "bad")), RunError);

  auto later = stage_at(40);
  later.add_shocks = {FundamentalJump{.at = SimTime{45 * kSecond}, .delta_milliticks = 50'000}};
  const auto second = family.fork("chained", later, nullptr, first);
  const auto chained = report(first, second);
  ASSERT_FALSE(chained.is_null());
  EXPECT_EQ(chained.at("stage"), 1);
  EXPECT_EQ(load_config(second / "config.json").forks.size(), 2U);
  EXPECT_TRUE(verify_replay(second).verified);
}

TEST(ForkRunTest, RejectsStagesThatCouldChangeThePast) {
  const Family family{parent_config()};
  const auto rejected = [&](ForkStage stage) {
    try {
      static_cast<void>(fork_of(family.parent, std::move(stage), "bad"));
    } catch (const RunError& error) {
      return std::string{error.code()};
    }
    return std::string{"no error"};
  };
  auto early = stage_at(15);
  early.add_shocks = {FundamentalJump{.at = SimTime{10 * kSecond}, .delta_milliticks = 1}};
  EXPECT_EQ(rejected(early), "InvalidConfig");
  auto started = stage_at(25);
  started.remove_shocks = {0};
  EXPECT_EQ(rejected(started), "InvalidConfig");
  auto unknown = stage_at(15);
  unknown.remove_shocks = {3};
  EXPECT_EQ(rejected(unknown), "InvalidConfig");
  EXPECT_EQ(rejected(stage_at(60)), "InvalidConfig"); // at the horizon
  auto longer = stage_at(15);
  longer.horizon = SimTime{90 * kSecond};
  EXPECT_EQ(rejected(longer), "InvalidConfig");
  auto directive = stage_at(10);
  directive.directive_overrides = {{.agent = AgentId{1}, .directive = Directive{}}};
  EXPECT_EQ(rejected(directive), "InvalidConfig"); // agent 1 is not a desk

  // A config that does not lead back to its parent's digest.
  auto config = fork_of(family.parent, stage_at(15), "tampered");
  config.seed += 1;
  EXPECT_EQ(error_code(config), "ForkParentMismatch");
  // Stages out of time order.
  auto twice = fork_of(family.parent, stage_at(15), "twice");
  auto earlier = stage_at(5);
  earlier.parent_run_id = "twice";
  earlier.parent_config_digest = sha256(canonical_config(twice));
  twice.forks.push_back(earlier);
  EXPECT_EQ(error_code(twice), "InvalidConfig");

  // A verified run that is not the named parent.
  auto other = parent_config();
  other.seed = 99;
  Simulation stranger{other, family.directory.path() / "stranger"};
  static_cast<void>(stranger.run());
  const auto fork = family.fork("empty", stage_at(15));
  EXPECT_EQ(verify_fork(family.directory.path() / "stranger", fork).code, "ParentMismatch");
  EXPECT_EQ(verify_fork(fork, fork).code, "ParentMismatch");
}

// Fails every decision, as an unreachable provider would.
class FailingSource final : public DecisionSource {
public:
  DecisionResponse decide(const DecisionRequest& /*request*/) override {
    ++calls;
    return {
        .directive = {},
        .source = "model",
        .model = "down",
        .input_tokens = 0,
        .output_tokens = 0,
        .error_code = "ProviderUnavailable",
    };
  }
  int calls = 0;
};

// Answers with a directive outside the desk's bounds.
class ReckelessSource final : public DecisionSource {
public:
  DecisionResponse decide(const DecisionRequest& request) override {
    auto directive = *request.current;
    directive.half_spread_ticks = 1'000;
    return {
        .directive = directive,
        .source = "model",
        .model = "reckless",
        .input_tokens = 7,
        .output_tokens = 3,
        .error_code = {},
    };
  }
};

TEST(ForkRunTest, ADeskRunRecordsEveryDecisionAndReplaysWithoutItsSource) {
  const auto config = desk_config();
  test::CaptureSink sink;
  EXPECT_THROW(static_cast<void>(run_kernel(config, sink)), RunError);

  FixtureDecisionSource fixture;
  const Family family{config, &fixture};
  const auto records = decisions(family.parent);
  ASSERT_EQ(records.size(), 6U); // 10 s to 60 s
  for (std::size_t index = 0; index < records.size(); ++index) {
    const auto& record = records.at(index);
    EXPECT_EQ(record.at("timestamp_ns"), (index + 1) * 10 * kSecond);
    EXPECT_EQ(record.at("actor_id"), 1);
    const auto& payload = record.at("payload");
    EXPECT_EQ(payload.at("source"), "model");
    EXPECT_EQ(payload.at("model"), "fixture");
    EXPECT_EQ(payload.at("index"), index + 1);
    EXPECT_EQ(payload.at("observation_digest"), sha256(payload.at("observation").dump()));
    // Only what the agent was delivered: no fundamental, no other agents, no shocks.
    const auto text = payload.at("observation").dump();
    EXPECT_EQ(text.find("fundamental"), std::string::npos);
    EXPECT_EQ(text.find("shock"), std::string::npos);
  }
  EXPECT_TRUE(verify_replay(family.parent).verified);

  // A failing or reckless source keeps the previous directive and says so.
  FailingSource failing;
  const Family down{config, &failing};
  EXPECT_EQ(failing.calls, 6);
  for (const auto& record : decisions(down.parent)) {
    EXPECT_EQ(record.at("payload").at("source"), "fallback");
    EXPECT_EQ(record.at("payload").at("error_code"), "ProviderUnavailable");
    EXPECT_EQ(record.at("payload").at("directive").at("rationale"), "start");
  }
  EXPECT_TRUE(verify_replay(down.parent).verified);
  ReckelessSource reckless;
  const Family wild{config, &reckless};
  const auto bounded = decisions(wild.parent);
  EXPECT_EQ(bounded.front().at("payload").at("error_code"), "InvalidDirective");
  EXPECT_EQ(bounded.front().at("payload").at("input_tokens"), 7);
  EXPECT_TRUE(verify_replay(wild.parent).verified);
}

TEST(ForkRunTest, DeskForksHoldReplaceOrReplayTheParentsDecisions) {
  FixtureDecisionSource fixture;
  const Family family{desk_config(), &fixture};
  const auto parent = decisions(family.parent);

  const auto hold = stage_at(30);
  const auto held = decisions(family.fork("hold", hold));
  ASSERT_EQ(held.size(), parent.size());
  for (std::size_t index = 0; index < held.size(); ++index) {
    if (index < 2) {
      EXPECT_EQ(held.at(index).at("payload"), parent.at(index).at("payload")); // inherited
    } else {
      EXPECT_EQ(held.at(index).at("payload").at("source"), "hold");
      EXPECT_EQ(held.at(index).at("payload").at("directive"),
                parent.at(1).at("payload").at("directive"));
    }
  }

  // The decision ledger's shape: the decision at 30 s replaced by the one it superseded,
  // the parent's later decisions kept, and the fork stopped at the next decision.
  auto ledger = stage_at(30);
  ledger.decisions = DecisionMode::Parent;
  ledger.horizon = SimTime{40 * kSecond};
  ledger.directive_overrides = {
      {
          .agent = AgentId{1},
          .directive = parse_directive(parent.at(1).at("payload").at("directive").dump()),
      },
  };
  const auto fork = family.fork("ledger", ledger);
  const auto replaced = decisions(fork);
  ASSERT_EQ(replaced.size(), 4U);
  EXPECT_EQ(replaced.at(2).at("payload").at("source"), "override");
  EXPECT_EQ(replaced.at(3).at("payload").at("source"), "parent");
  EXPECT_EQ(replaced.at(3).at("payload").at("directive"),
            parent.at(3).at("payload").at("directive"));
  const auto result = report(family.parent, fork);
  ASSERT_FALSE(result.is_null());
  EXPECT_EQ(result.at("window").at("end_ns"), 40 * kSecond);

  // A live fork asks the source again after the stage, and only then.
  FailingSource failing;
  auto live = stage_at(30);
  live.decisions = DecisionMode::Live;
  live.add_shocks = {QuoteWithdrawalStress{.at = SimTime{30 * kSecond}, .agents = {AgentId{2}}}};
  const auto relived = decisions(family.fork("live", live, &failing));
  EXPECT_EQ(failing.calls, 4);
  EXPECT_EQ(relived.at(1).at("payload").at("source"), "model");
  EXPECT_EQ(relived.at(2).at("payload").at("source"), "fallback");
}

TEST(ForkRunTest, AForkVariantPairsEverySeedWithItsOwnParentCell) {
  ExperimentConfig config;
  config.experiment_id = "stress";
  config.base = parent_config();
  config.horizon = SimTime{30 * kSecond};
  config.seeds = {3, 4, 5};
  auto stage = stage_at(15);
  stage.add_shocks = {QuoteWithdrawalStress{.at = SimTime{15 * kSecond}, .agents = {AgentId{1}}}};
  config.variants = {
      {.name = "calm", .overrides = {}, .fork = std::nullopt},
      {.name = "stressed", .overrides = {}, .fork = VariantFork{.parent = "calm", .stage = stage}},
  };
  config.comparison =
      ComparisonConfig{.baseline = "calm", .candidate = "stressed", .subject = AgentId{1}};
  config.max_runs = 6;
  const auto canonical = Json::parse(canonical_experiment(config));
  EXPECT_EQ(canonical.at("schema_version"), 2);
  EXPECT_EQ(canonical.at("variants").at(1).at("fork").at("parent"), "calm");
  EXPECT_EQ(canonical_experiment(parse_experiment(canonical.dump(), {})), canonical.dump());

  for (const auto seed : config.seeds) {
    const auto parent = variant_config(config, config.variants.at(0), seed);
    const auto cell = variant_config(config, config.variants.at(1), seed);
    ASSERT_EQ(cell.forks.size(), 1U);
    EXPECT_EQ(cell.forks.front().parent_run_id, parent.run_id);
    EXPECT_EQ(cell.forks.front().parent_config_digest, sha256(canonical_config(parent)));
  }
  const auto result = run_experiment(config, {});
  ASSERT_EQ(result.runs.size(), 6U);
  for (const auto& run : result.runs) {
    EXPECT_TRUE(run.completed) << run.error_code << ": " << run.error_message;
  }
  EXPECT_FALSE(result.comparisons.empty());

  // A fork of a fork variant, or of an unknown one, is rejected.
  auto chained = config;
  chained.variants.push_back({
      .name = "again",
      .overrides = {},
      .fork = VariantFork{.parent = "stressed", .stage = stage},
  });
  chained.max_runs = 9;
  EXPECT_THROW(validate_experiment(chained), RunError);
}

} // namespace
} // namespace markettwin
