#include "markettwin/common/hash.hpp"
#include "markettwin/experiment/experiment.hpp"
#include "markettwin/telemetry/replay.hpp"

#include "../test_support.hpp"

#include <gtest/gtest.h>
#include <set>

namespace markettwin {
namespace {
using Json = nlohmann::json;
using test::kSecond;

constexpr MarketMakerParams kQuoting{
    .half_spread_ticks = 2,
    .quote_size = Quantity{100},
    .levels = 2,
    .level_spacing_ticks = 1,
};

// Agent 2 of the scenario fixture is the subject. Everything else in the market, and
// every random stream, is the same in both variants.
ExperimentConfig fixture() {
  ExperimentConfig config;
  config.experiment_id = "makers";
  config.base = test::scenario_fixture();
  config.horizon = SimTime{20 * kSecond};
  config.seeds = {11, 12, 13};
  const PolicyConfig inventory = InventoryMarketMakerParams{.quoting = kQuoting};
  config.variants = {
      {.name = "plain", .overrides = {{.agent = AgentId{2}, .policy = kQuoting}}},
      {.name = "inventory", .overrides = {{.agent = AgentId{2}, .policy = inventory}}},
  };
  config.comparison =
      ComparisonConfig{.baseline = "plain", .candidate = "inventory", .subject = AgentId{2}};
  config.bootstrap = {.resamples = 200, .seed = 5};
  config.max_runs = 6;
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

std::string invalid(const std::function<void(ExperimentConfig&)>& mutate) {
  auto config = fixture();
  mutate(config);
  return code_of([&] { validate_experiment(config); });
}

const MetricComparison& metric(const ExperimentResult& result, std::string_view name) {
  for (const auto& item : result.comparisons) {
    if (item.name == name) {
      return item;
    }
  }
  throw std::logic_error("no such metric");
}

const AgentReport& subject(const RunOutcome& run) {
  for (const auto& agent : run.summary.report.agents) {
    if (agent.account.agent == AgentId{2}) {
      return agent;
    }
  }
  throw std::logic_error("no subject");
}

std::int64_t total_pnl(const RunOutcome& run) {
  const auto& valuation = subject(run).valuation;
  if (!valuation) {
    throw std::logic_error("subject has no valuation");
  }
  return valuation->total_pnl.value();
}

Summary present(const std::optional<Summary>& summary) {
  if (!summary) {
    throw std::logic_error("expected a summary");
  }
  return *summary;
}

TEST(ExperimentTest, RepeatingAnExperimentReproducesEveryRunAndEveryStatistic) {
  const auto config = fixture();
  const auto first = run_experiment(config);
  const auto second = run_experiment(config);
  EXPECT_EQ(first, second);
  EXPECT_EQ(experiment_results_json(config, first), experiment_results_json(config, second));
  ASSERT_EQ(first.runs.size(), 6U);
  // Seed-major order, with the variants in their configured order inside each seed.
  for (std::size_t index = 0; index < first.runs.size(); ++index) {
    const auto& run = first.runs.at(index);
    EXPECT_TRUE(run.completed);
    EXPECT_EQ(run.seed, config.seeds.at(index / 2));
    EXPECT_EQ(run.variant, config.variants.at(index % 2).name);
    EXPECT_EQ(run.run_id, "makers-" + run.variant + "-s" + std::to_string(run.seed));
    EXPECT_EQ(run.summary.final_time, SimTime{20 * kSecond});
    // A cell is an ordinary run: running its config alone gives the same summary.
    EXPECT_EQ(Simulation{variant_config(config, config.variants.at(index % 2), run.seed)}.run(),
              run.summary);
  }
  EXPECT_EQ(first.counts.at(0), (VariantCount{.variant = "plain", .completed = 3, .failed = 0}));
  EXPECT_EQ(first.counts.at(1).completed, 3U);
  EXPECT_EQ(first.pairs_completed, 3U);
}

TEST(ExperimentTest, SeedsDifferWhileExogenousProcessesStayPairedAcrossVariants) {
  const auto config = fixture();
  const auto result = run_experiment(config);
  std::set<std::string> traces;
  for (const auto& run : result.runs) {
    traces.insert(run.summary.trace_digest);
  }
  EXPECT_EQ(traces.size(), 6U); // every seed and every variant is its own market
  for (std::size_t seed = 0; seed < config.seeds.size(); ++seed) {
    const auto& plain = test::scenario(result.runs.at(2 * seed).summary);
    const auto& inventory = test::scenario(result.runs.at((2 * seed) + 1).summary);
    // The latent value is exogenous: the same path whatever the subject does.
    EXPECT_EQ(plain.fundamental_milliticks, inventory.fundamental_milliticks);
    EXPECT_EQ(plain.fundamental_updates, inventory.fundamental_updates);
    // Each agent wakes on its own stream, so every agent wakes equally often in both.
    for (std::size_t agent = 0; agent < plain.agents.size(); ++agent) {
      EXPECT_EQ(plain.agents.at(agent).wakeups, inventory.agents.at(agent).wakeups);
    }
    // Only the subject's policy differs between the two configs.
    EXPECT_EQ(plain.agents.at(1).policy, "MarketMaker");
    EXPECT_EQ(inventory.agents.at(1).policy, "InventoryMarketMaker");
    EXPECT_EQ(plain.agents.at(0).policy, inventory.agents.at(0).policy);
    // What happens downstream is endogenous and does diverge.
    EXPECT_NE(plain.rng_digest.empty(), true);
    if (seed > 0) {
      EXPECT_NE(plain.fundamental_milliticks,
                test::scenario(result.runs.at(2 * (seed - 1)).summary).fundamental_milliticks);
    }
  }
}

TEST(ExperimentTest, PairedDifferencesAreCandidateMinusBaselineSeedBySeed) {
  const auto config = fixture();
  const auto result = run_experiment(config);
  const auto& pnl = metric(result, "total_pnl");
  EXPECT_EQ(pnl.unit, "minor");
  EXPECT_EQ(pnl.scope, "subject");
  EXPECT_EQ(pnl.pairs, 3U);
  EXPECT_EQ(pnl.pairs_missing, 0U);
  std::vector<std::int64_t> baseline;
  std::vector<std::int64_t> candidate;
  std::vector<std::int64_t> differences;
  for (std::size_t seed = 0; seed < 3; ++seed) {
    baseline.push_back(total_pnl(result.runs.at(2 * seed)));
    candidate.push_back(total_pnl(result.runs.at((2 * seed) + 1)));
    differences.push_back(candidate.back() - baseline.back());
  }
  EXPECT_EQ(pnl.baseline, summarize(baseline));
  EXPECT_EQ(pnl.candidate, summarize(candidate));
  const auto index = static_cast<std::uint64_t>(&pnl - result.comparisons.data());
  EXPECT_EQ(pnl.difference, summarize(differences, config.bootstrap, index));
  const auto difference = present(pnl.difference);
  EXPECT_TRUE(difference.mean_t_interval.has_value());
  EXPECT_TRUE(difference.mean_bootstrap_interval.has_value());
  // The mean of the differences is the difference of the means.
  EXPECT_NEAR(
      static_cast<double>(difference.mean_milli),
      static_cast<double>(present(pnl.candidate).mean_milli - present(pnl.baseline).mean_milli),
      1.0);
  EXPECT_EQ(result.comparisons.size(), 19U);
  for (const auto& item : result.comparisons) {
    EXPECT_EQ(item.pairs + item.pairs_missing, 3U) << item.name;
    EXPECT_TRUE(item.scope == "subject" || item.scope == "market") << item.name;
  }
  EXPECT_EQ(metric(result, "fees_paid").difference,
            summarize(std::vector<std::int64_t>{0, 0, 0}, config.bootstrap, 3));
}

TEST(ExperimentTest, IdenticalVariantsShowNoDifferenceAtAll) {
  auto config = fixture();
  config.variants.at(1).overrides = config.variants.at(0).overrides;
  const auto result = run_experiment(config);
  for (std::size_t seed = 0; seed < 3; ++seed) {
    const auto& first = result.runs.at(2 * seed).summary;
    const auto& second = result.runs.at((2 * seed) + 1).summary;
    EXPECT_EQ(first.trade_digest, second.trade_digest);
    EXPECT_EQ(first.report, second.report);
    EXPECT_NE(first.config_digest, second.config_digest); // the run IDs differ
  }
  for (const auto& item : result.comparisons) {
    if (item.pairs == 0) {
      continue;
    }
    const auto difference = present(item.difference);
    EXPECT_EQ(difference.minimum, 0) << item.name;
    EXPECT_EQ(difference.maximum, 0) << item.name;
    EXPECT_EQ(difference.mean_t_interval, (Interval{0, 0})) << item.name;
  }
  EXPECT_NE(experiment_report(config, result).find("identical"), std::string::npos);
}

TEST(ExperimentTest, RejectsEmptyOversizedAndInconsistentDesigns) {
  EXPECT_EQ(invalid([](ExperimentConfig&) {}), "no error");
  EXPECT_EQ(invalid([](ExperimentConfig& config) { config.seeds.clear(); }), "InvalidConfig");
  EXPECT_EQ(invalid([](ExperimentConfig& config) { config.variants.clear(); }), "InvalidConfig");
  EXPECT_EQ(invalid([](ExperimentConfig& config) { config.seeds = {1, 2, 1}; }), "InvalidConfig");
  EXPECT_EQ(invalid([](ExperimentConfig& config) { config.max_runs = 0; }), "InvalidConfig");
  // Three seeds and two variants are six runs, not three.
  EXPECT_EQ(invalid([](ExperimentConfig& config) { config.max_runs = 5; }), "InvalidConfig");
  EXPECT_EQ(invalid([](ExperimentConfig& config) { config.max_runs = kMaxExperimentRuns + 1; }),
            "InvalidConfig");
  EXPECT_EQ(invalid([](ExperimentConfig& config) { config.experiment_id = "bad id"; }),
            "InvalidConfig");
  EXPECT_EQ(invalid([](ExperimentConfig& config) { config.experiment_id.clear(); }),
            "InvalidConfig");
  EXPECT_EQ(invalid([](ExperimentConfig& config) { config.variants.at(1).name = "plain"; }),
            "InvalidConfig");
  EXPECT_EQ(invalid([](ExperimentConfig& config) { config.variants.at(1).name = "../x"; }),
            "InvalidConfig");
  EXPECT_EQ(invalid([](ExperimentConfig& config) {
              config.variants.at(0).overrides.front().agent = AgentId{99};
            }),
            "InvalidConfig");
  EXPECT_EQ(invalid([](ExperimentConfig& config) {
              auto& overrides = config.variants.at(0).overrides;
              overrides.push_back(overrides.front());
            }),
            "InvalidConfig");
  // An override must leave a valid run config: 20 levels do not fit 16 open orders.
  EXPECT_EQ(invalid([](ExperimentConfig& config) {
              auto quoting = kQuoting;
              quoting.levels = 20;
              config.variants.at(0).overrides.front().policy = quoting;
            }),
            "InvalidConfig");
  EXPECT_EQ(invalid([](ExperimentConfig& config) { config.comparison->candidate = "absent"; }),
            "InvalidConfig");
  EXPECT_EQ(invalid([](ExperimentConfig& config) { config.comparison->candidate = "plain"; }),
            "InvalidConfig");
  EXPECT_EQ(invalid([](ExperimentConfig& config) { config.comparison->subject = AgentId{99}; }),
            "InvalidConfig");
  EXPECT_EQ(invalid([](ExperimentConfig& config) { config.bootstrap.resamples = 50; }),
            "InvalidConfig");
  EXPECT_EQ(invalid([](ExperimentConfig& config) { config.bootstrap.resamples = 0; }), "no error");
  EXPECT_EQ(invalid([](ExperimentConfig& config) { config.artifacts = ArtifactPolicy::All; }),
            "InvalidConfig"); // no output budget
  EXPECT_EQ(invalid([](ExperimentConfig& config) { config.base.scenario.reset(); }),
            "InvalidConfig");
  EXPECT_EQ(invalid([](ExperimentConfig& config) { config.comparison.reset(); }), "no error");
  // Keeping run directories needs somewhere to put them.
  auto keep = fixture();
  keep.artifacts = ArtifactPolicy::All;
  keep.max_output_bytes = 1'000'000'000;
  EXPECT_EQ(code_of([&] { static_cast<void>(run_experiment(keep)); }), "InvalidConfig");
}

TEST(ExperimentTest, AFailedRunIsReportedAndOnlyCompletePairsAreCompared) {
  const auto config = fixture();
  const RunFunction flaky = [](const RunConfig& run, const std::filesystem::path& directory) {
    if (run.run_id == "makers-inventory-s12") {
      throw RunError{"Injected", "candidate failed"};
    }
    if (run.run_id == "makers-plain-s13") {
      throw std::runtime_error{"baseline failed"};
    }
    return Simulation{run, directory}.run();
  };
  const auto result = run_experiment(config, {}, flaky);
  ASSERT_EQ(result.runs.size(), 6U); // nothing was skipped
  EXPECT_EQ(result.counts.at(0), (VariantCount{.variant = "plain", .completed = 2, .failed = 1}));
  EXPECT_EQ(result.counts.at(1),
            (VariantCount{.variant = "inventory", .completed = 2, .failed = 1}));
  EXPECT_FALSE(result.runs.at(3).completed);
  EXPECT_EQ(result.runs.at(3).error_code, "Injected");
  EXPECT_EQ(result.runs.at(3).error_message, "candidate failed");
  EXPECT_EQ(result.runs.at(4).error_code, "RunFailed");
  EXPECT_TRUE(result.runs.at(5).completed); // the run after a failure still executes
  // Only seed 11 has both sides. One pair has a mean and no interval.
  EXPECT_EQ(result.pairs_completed, 1U);
  const auto& pnl = metric(result, "total_pnl");
  EXPECT_EQ(pnl.pairs, 1U);
  const auto difference = present(pnl.difference);
  EXPECT_EQ(difference.count, 1U);
  EXPECT_EQ(difference.mean_milli,
            (total_pnl(result.runs.at(1)) - total_pnl(result.runs.at(0))) * 1'000);
  EXPECT_FALSE(difference.standard_deviation_milli.has_value());
  EXPECT_FALSE(difference.mean_t_interval.has_value());

  const auto document = Json::parse(experiment_results_json(config, result));
  const auto& failed = document.at("runs").at(3);
  EXPECT_EQ(failed.at("status"), "Failed");
  EXPECT_EQ(failed.at("error_code"), "Injected");
  EXPECT_TRUE(failed.at("trace_digest").is_null());
  EXPECT_EQ(document.at("runs").at(0).at("status"), "Completed");
  EXPECT_TRUE(document.at("runs").at(0).at("error_code").is_null());
  EXPECT_EQ(document.at("comparison").at("pairs_completed"), 1);
  const auto report = experiment_report(config, result);
  EXPECT_NE(report.find("makers-inventory-s12: Injected: candidate failed"), std::string::npos);
  EXPECT_NE(report.find("2 completed, 1 failed"), std::string::npos);

  // With every candidate failing there is nothing to compare, and it says so.
  const RunFunction broken = [](const RunConfig& run, const std::filesystem::path& directory) {
    if (run.run_id.find("inventory") != std::string::npos) {
      throw RunError{"Injected", "always"};
    }
    return Simulation{run, directory}.run();
  };
  const auto empty = run_experiment(config, {}, broken);
  EXPECT_EQ(empty.pairs_completed, 0U);
  EXPECT_EQ(metric(empty, "total_pnl").pairs, 0U);
  EXPECT_FALSE(metric(empty, "total_pnl").difference.has_value());
  EXPECT_NE(experiment_report(config, empty).find("n/a"), std::string::npos);
}

TEST(ExperimentTest, WritesVersionedResultsAndReplayableRunDirectoriesWithoutOverwriting) {
  const test::TempDirectory root;
  auto config = fixture();
  config.seeds = {11, 12};
  config.max_runs = 4;
  config.artifacts = ArtifactPolicy::All;
  config.max_output_bytes = 1'000'000'000;
  const auto output = root.path() / "experiment";
  const auto result = run_experiment(config, output);

  const auto document = Json::parse(test::read(output / "experiment.json"));
  EXPECT_EQ(document.at("schema_version"), kExperimentSchema);
  EXPECT_EQ(document.at("engine_version"), MARKETTWIN_TEST_VERSION);
  EXPECT_TRUE(document.at("provenance").contains("git_revision"));
  const auto results = experiment_results_json(config, result);
  EXPECT_EQ(document.at("results"), Json::parse(results));
  EXPECT_EQ(document.at("results_digest"), sha256(results));
  EXPECT_EQ(document.at("artifacts").at("runs.jsonl"), sha256(test::read(output / "runs.jsonl")));
  EXPECT_EQ(document.at("results").at("config").at("seeds"), (Json{11, 12}));
  EXPECT_EQ(document.at("results").at("design").at("total_runs"), 4);

  std::istringstream lines{test::read(output / "runs.jsonl")};
  std::string line;
  std::size_t index = 0;
  while (std::getline(lines, line)) {
    const auto& run = result.runs.at(index);
    const auto entry = Json::parse(line);
    const auto cell = variant_config(config, config.variants.at(index % 2), run.seed);
    EXPECT_EQ(entry.at("run_id"), run.run_id);
    EXPECT_EQ(entry.at("summary"), Json::parse(summary_json(run.summary)));
    EXPECT_EQ(entry.at("metrics"), Json::parse(metrics_json(cell, run.summary)));
    // Every run directory is an ordinary run that replay verifies on its own.
    ASSERT_FALSE(run.artifacts.empty());
    const auto directory = output / run.artifacts;
    const auto replay = verify_replay(directory);
    EXPECT_TRUE(replay.verified) << replay_result_json(replay);
    EXPECT_EQ(replay.summary, run.summary);
    const auto& listed = document.at("results").at("runs").at(index);
    EXPECT_EQ(listed.at("artifacts"), run.artifacts);
    EXPECT_EQ(listed.at("trace_digest"), run.summary.trace_digest);
    EXPECT_EQ(listed.at("accounting_digest"), run.summary.accounting_digest);
    EXPECT_EQ(listed.at("metrics_digest"), sha256(test::read(directory / "metrics.json")));
    ++index;
  }
  EXPECT_EQ(index, 4U);

  // A finished experiment is never overwritten, and its files are left exactly as they were.
  const auto before = test::read(output / "experiment.json");
  EXPECT_EQ(code_of([&] { static_cast<void>(run_experiment(config, output)); }), "OutputExists");
  EXPECT_EQ(test::read(output / "experiment.json"), before);

  // In-memory runs produce the same deterministic results; only the artifact paths differ.
  auto memory = config;
  memory.artifacts = ArtifactPolicy::None;
  const auto summary_only = run_experiment(memory, root.path() / "summary-only");
  EXPECT_EQ(summary_only.comparisons, result.comparisons);
  EXPECT_FALSE(std::filesystem::exists(root.path() / "summary-only" / "runs"));
  EXPECT_TRUE(summary_only.runs.front().artifacts.empty());
  EXPECT_EQ(summary_only.runs.front().summary, result.runs.front().summary);
}

TEST(ExperimentTest, TheOutputBudgetStopsWritingRunsAndReportsTheRest) {
  const test::TempDirectory root;
  auto config = fixture();
  config.artifacts = ArtifactPolicy::All;
  config.max_output_bytes = 1; // the first run already exceeds it
  const auto result = run_experiment(config, root.path() / "experiment");
  EXPECT_TRUE(result.runs.front().completed);
  for (std::size_t index = 1; index < result.runs.size(); ++index) {
    EXPECT_FALSE(result.runs.at(index).completed);
    EXPECT_EQ(result.runs.at(index).error_code, "OutputLimit");
  }
  EXPECT_EQ(result.pairs_completed, 0U);
  EXPECT_TRUE(std::filesystem::exists(root.path() / "experiment" / "experiment.json"));
  EXPECT_FALSE(std::filesystem::exists(root.path() / "experiment/runs/inventory/seed-11"));
}

TEST(ExperimentTest, ConfigParsingIsStrictAndCanonicalFormRoundTrips) {
  const auto config = fixture();
  const auto canonical = canonical_experiment(config);
  const auto reparsed = parse_experiment(canonical, {});
  EXPECT_EQ(canonical_experiment(reparsed), canonical);
  EXPECT_EQ(reparsed.seeds, config.seeds);
  EXPECT_EQ(reparsed.variants, config.variants);
  EXPECT_EQ(reparsed.comparison, config.comparison);
  EXPECT_EQ(reparsed.horizon, config.horizon);
  EXPECT_EQ(run_experiment(reparsed), run_experiment(config));

  const auto valid = Json::parse(canonical);
  const auto rejected = [&](const std::function<void(Json&)>& mutate) {
    auto changed = valid;
    mutate(changed);
    return code_of([&] { static_cast<void>(parse_experiment(changed.dump(), {})); });
  };
  EXPECT_EQ(rejected([](Json& value) { value.at("schema_version") = 2; }), "UnsupportedSchema");
  EXPECT_EQ(rejected([](Json& value) { value.emplace("extra", 1); }), "InvalidConfig");
  EXPECT_EQ(rejected([](Json& value) { value.erase("limits"); }), "InvalidConfig");
  EXPECT_EQ(rejected([](Json& value) { value.at("seeds") = Json::array(); }), "InvalidConfig");
  EXPECT_EQ(rejected([](Json& value) { value.at("seeds") = {1.5}; }), "InvalidConfig");
  EXPECT_EQ(rejected([](Json& value) { value.at("artifacts") = "Some"; }), "InvalidConfig");
  EXPECT_EQ(rejected([](Json& value) {
              value.at("variants").at(0).at("policy_overrides").at(0).at("policy").at("kind") =
                  "Clairvoyant";
            }),
            "InvalidConfig");
  EXPECT_EQ(rejected([](Json& value) { value.at("base_config") = "missing.json"; }), "IoError");
  EXPECT_EQ(code_of([] { static_cast<void>(parse_experiment("{", {})); }), "InvalidJson");

  // A seed range is shorthand for the consecutive list it names.
  auto ranged = valid;
  ranged.at("seeds") = {{"first", 40}, {"count", 3}};
  EXPECT_EQ(parse_experiment(ranged.dump(), {}).seeds, (std::vector<std::uint64_t>{40, 41, 42}));
  ranged.at("seeds") = {{"first", 40}, {"count", 0}};
  EXPECT_EQ(code_of([&] { static_cast<void>(parse_experiment(ranged.dump(), {})); }),
            "InvalidConfig");
  ranged.at("seeds") = {{"first", ~std::uint64_t{0}}, {"count", 2}};
  EXPECT_EQ(code_of([&] { static_cast<void>(parse_experiment(ranged.dump(), {})); }),
            "InvalidConfig");
}

TEST(ExperimentTest, TheShippedMakerComparisonReplacesOnePolicyAndKeepsItsQuotingParameters) {
  const std::filesystem::path source{MARKETTWIN_SOURCE_DIR};
  const auto config = load_experiment(source / "configs/experiments/maker_comparison.json");
  EXPECT_EQ(config.seeds.size(), 200U);
  EXPECT_EQ(config.seeds.front(), 1U);
  EXPECT_EQ(config.seeds.back(), 200U);
  ASSERT_EQ(config.variants.size(), 2U);
  ASSERT_TRUE(config.comparison.has_value());
  const auto subject_id = config.comparison->subject;
  EXPECT_EQ(subject_id, AgentId{3});
  const auto plain = variant_config(config, config.variants.at(0), 1);
  const auto inventory = variant_config(config, config.variants.at(1), 1);
  const auto& before = test::scenario(plain).agents;
  const auto& after = test::scenario(inventory).agents;
  ASSERT_EQ(before.size(), after.size());
  for (std::size_t index = 0; index < before.size(); ++index) {
    if (before.at(index).id != subject_id) {
      EXPECT_EQ(before.at(index), after.at(index)); // every other participant is identical
      continue;
    }
    // The subject keeps its identity, schedule, latency, and limits ...
    EXPECT_EQ(before.at(index).wakeup, after.at(index).wakeup);
    EXPECT_EQ(before.at(index).latency, after.at(index).latency);
    EXPECT_EQ(before.at(index).limits, after.at(index).limits);
    // ... and the same quoting core; only the inventory control is added.
    EXPECT_EQ(std::get<MarketMakerParams>(before.at(index).policy),
              std::get<InventoryMarketMakerParams>(after.at(index).policy).quoting);
  }
  // The candidate variant is the shipped baseline scenario itself.
  const auto baseline = load_config(source / "configs/baseline.json");
  EXPECT_EQ(test::scenario(inventory), test::scenario(baseline));
  const auto smoke = load_experiment(source / "configs/experiments/maker_comparison_smoke.json");
  EXPECT_EQ(smoke.variants, config.variants);
  EXPECT_EQ(smoke.artifacts, ArtifactPolicy::All);
}

// Integer statistics over integer runs: the whole results document is pinned, and CI
// holds it on Apple Clang and on GCC.
TEST(ExperimentTest, ResultsAreByteStableAndMatchTheGoldenFixtureOnEveryToolchain) {
  const std::filesystem::path source{MARKETTWIN_SOURCE_DIR};
  const auto config = fixture();
  const auto results = experiment_results_json(config, run_experiment(config));
  EXPECT_EQ(Json::parse(results).dump(), results);  // canonical: sorted keys, integers only
  EXPECT_EQ(results.find('.'), results.find(".0")); // no fractional number anywhere
  const auto golden =
      Json::parse(test::read(source / "core/tests/replay/golden/phase2b_experiment_results.json"));
  EXPECT_EQ(Json::parse(results), golden);
}

TEST(ExperimentTest, TheTerminalReportStatesDesignUnitsAndCaveats) {
  const auto config = fixture();
  const auto report = experiment_report(config, run_experiment(config));
  for (const auto* expected : {
           "MarketTwin experiment makers",
           "3 seeds x 2 variants = 6 runs, paired by seed",
           "inventory minus plain for agent 2, 3 paired seeds",
           "subject.total_pnl",
           "minor",
           "market.mean_spread",
           "milliticks",
           "95% t interval",
           "inconclusive",
           "not evidence that a strategy is",
           "no multiplicity correction",
       }) {
    EXPECT_NE(report.find(expected), std::string::npos) << expected;
  }
}

} // namespace
} // namespace markettwin
