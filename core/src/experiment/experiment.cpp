#include "markettwin/experiment/experiment.hpp"

#include "markettwin/common/hash.hpp"
#include "markettwin/common/version.hpp"

#include "../telemetry/artifacts.hpp"
#include "../telemetry/codec.hpp"

#include <fstream>
#include <set>

namespace markettwin {
namespace {
using codec::Json;

constexpr std::size_t kMaxNameLength = 48;
constexpr std::array<std::string_view, 2> kArtifactPolicies{"None", "All"};

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw RunError{"InvalidConfig", message};
  }
}

bool valid_name(std::string_view name) {
  return !name.empty() && name.size() <= kMaxNameLength && std::ranges::all_of(name, [](char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
           ch == '_' || ch == '-';
  });
}

const Variant* find_variant(const ExperimentConfig& config, std::string_view name) {
  const auto found = std::ranges::find(config.variants, name, &Variant::name);
  return found == config.variants.end() ? nullptr : &*found;
}

// ---- metrics compared across variants ----------------------------------------------

using Extract = std::optional<std::int64_t> (*)(const RunReport&, const AgentReport&);

class MetricDefinition {
public:
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): scope, name, unit, as in the table.
  constexpr MetricDefinition(std::string_view metric_scope, std::string_view metric_name,
                             std::string_view metric_unit, Extract extractor) noexcept
      : name(metric_name), unit(metric_unit), scope(metric_scope), extract(extractor) {}

  std::string_view name;
  std::string_view unit;
  std::string_view scope;
  Extract extract;
};

using Value = std::optional<std::int64_t>;

Value count(std::uint64_t value) {
  return to_signed(value);
}

Value count(std::optional<std::uint64_t> value) {
  return value ? Value{to_signed(*value)} : std::nullopt;
}

Value valued(const AgentReport& agent, Money Valuation::* member) {
  return agent.valuation ? Value{((*agent.valuation).*member).value()} : std::nullopt;
}

Value total_pnl(const RunReport& /*run*/, const AgentReport& agent) {
  return valued(agent, &Valuation::total_pnl);
}
Value realized_pnl(const RunReport& /*run*/, const AgentReport& agent) {
  return agent.account.realized_pnl.value();
}
Value unrealized_pnl(const RunReport& /*run*/, const AgentReport& agent) {
  return valued(agent, &Valuation::unrealized_pnl);
}
Value fees_paid(const RunReport& /*run*/, const AgentReport& agent) {
  return agent.account.fees_paid.value();
}
Value max_drawdown(const RunReport& /*run*/, const AgentReport& agent) {
  return agent.max_drawdown ? Value{agent.max_drawdown->value()} : std::nullopt;
}
Value maker_spread_capture(const RunReport& /*run*/, const AgentReport& agent) {
  return agent.maker_spread_capture.value();
}
Value final_abs_position(const RunReport& /*run*/, const AgentReport& agent) {
  const auto position = agent.account.position;
  return position < 0 ? -position : position;
}
Value max_abs_position(const RunReport& /*run*/, const AgentReport& agent) {
  return count(agent.account.max_abs_position);
}
Value mean_abs_position(const RunReport& /*run*/, const AgentReport& agent) {
  return agent.mean_abs_position_millishares;
}
Value traded_quantity(const RunReport& /*run*/, const AgentReport& agent) {
  return count(checked_add(agent.account.bought, agent.account.sold).value());
}
Value fill_rate(const RunReport& /*run*/, const AgentReport& agent) {
  return count(agent.fill_rate_ppm);
}
Value trade_count(const RunReport& run, const AgentReport& /*agent*/) {
  return count(run.market.trade_count);
}
Value volume(const RunReport& run, const AgentReport& /*agent*/) {
  return count(run.market.volume.value());
}
Value mean_spread(const RunReport& run, const AgentReport& /*agent*/) {
  return run.market.mean_spread_milliticks;
}
Value mean_relative_spread(const RunReport& run, const AgentReport& /*agent*/) {
  return run.market.mean_relative_spread_millibps;
}
Value mean_bid_depth(const RunReport& run, const AgentReport& /*agent*/) {
  return run.market.mean_bid_depth_millishares;
}
Value mean_ask_depth(const RunReport& run, const AgentReport& /*agent*/) {
  return run.market.mean_ask_depth_millishares;
}
Value midpoint_return_volatility(const RunReport& run, const AgentReport& /*agent*/) {
  return count(run.market.midpoint_return_volatility_ppb);
}
Value two_sided_time(const RunReport& run, const AgentReport& /*agent*/) {
  if (run.market.total_ns == 0) {
    return std::nullopt;
  }
  return narrow(divide_round(static_cast<Int128>(run.market.two_sided_ns) * 1'000'000,
                             static_cast<Int128>(run.market.total_ns)));
}

// The compared metrics are fixed here, before any run, and all of them are always
// reported. "subject" metrics describe the compared agent, "market" ones the whole book.
constexpr std::array kMetrics{
    MetricDefinition("subject", "total_pnl", "minor", total_pnl),
    MetricDefinition("subject", "realized_pnl", "minor", realized_pnl),
    MetricDefinition("subject", "unrealized_pnl", "minor", unrealized_pnl),
    MetricDefinition("subject", "fees_paid", "minor", fees_paid),
    MetricDefinition("subject", "max_drawdown", "minor", max_drawdown),
    MetricDefinition("subject", "maker_spread_capture", "minor", maker_spread_capture),
    MetricDefinition("subject", "final_abs_position", "shares", final_abs_position),
    MetricDefinition("subject", "max_abs_position", "shares", max_abs_position),
    MetricDefinition("subject", "mean_abs_position", "millishares", mean_abs_position),
    MetricDefinition("subject", "traded_quantity", "shares", traded_quantity),
    MetricDefinition("subject", "fill_rate", "ppm", fill_rate),
    MetricDefinition("market", "trade_count", "trades", trade_count),
    MetricDefinition("market", "volume", "shares", volume),
    MetricDefinition("market", "mean_spread", "milliticks", mean_spread),
    MetricDefinition("market", "mean_relative_spread", "millibps", mean_relative_spread),
    MetricDefinition("market", "mean_bid_depth", "millishares", mean_bid_depth),
    MetricDefinition("market", "mean_ask_depth", "millishares", mean_ask_depth),
    MetricDefinition("market", "midpoint_return_volatility", "ppb", midpoint_return_volatility),
    MetricDefinition("market", "two_sided_time", "ppm", two_sided_time),
};

std::optional<std::int64_t> metric_value(const MetricDefinition& metric, const RunOutcome& run,
                                         AgentId subject) {
  const auto& agents = run.summary.report.agents;
  const auto found = std::ranges::find_if(
      agents, [&](const AgentReport& agent) { return agent.account.agent == subject; });
  if (found == agents.end()) {
    throw RunError{"InternalError", "run report has no account for the compared agent"};
  }
  return metric.extract(run.summary.report, *found);
}

std::vector<MetricComparison> compare(const ExperimentConfig& config,
                                      const std::vector<RunOutcome>& runs,
                                      std::uint64_t& pairs_completed) {
  std::vector<MetricComparison> result;
  if (!config.comparison) {
    return result;
  }
  const auto index_of = [&](std::string_view name) {
    return static_cast<std::size_t>(find_variant(config, name) - config.variants.data());
  };
  const auto baseline_index = index_of(config.comparison->baseline);
  const auto candidate_index = index_of(config.comparison->candidate);
  std::vector<std::pair<const RunOutcome*, const RunOutcome*>> pairs;
  for (std::size_t seed = 0; seed < config.seeds.size(); ++seed) {
    const auto& baseline = runs.at((seed * config.variants.size()) + baseline_index);
    const auto& candidate = runs.at((seed * config.variants.size()) + candidate_index);
    if (baseline.completed && candidate.completed) {
      pairs.emplace_back(&baseline, &candidate);
    }
  }
  pairs_completed = pairs.size();
  for (std::size_t index = 0; index < kMetrics.size(); ++index) {
    const auto& metric = kMetrics.at(index);
    MetricComparison comparison;
    comparison.name = metric.name;
    comparison.unit = metric.unit;
    comparison.scope = metric.scope;
    std::vector<std::int64_t> baseline;
    std::vector<std::int64_t> candidate;
    std::vector<std::int64_t> difference;
    for (const auto& [first, second] : pairs) {
      const auto before = metric_value(metric, *first, config.comparison->subject);
      const auto after = metric_value(metric, *second, config.comparison->subject);
      if (!before || !after) {
        ++comparison.pairs_missing;
        continue;
      }
      baseline.push_back(*before);
      candidate.push_back(*after);
      difference.push_back(narrow(Int128{*after} - Int128{*before}));
    }
    comparison.pairs = difference.size();
    comparison.baseline = summarize(baseline);
    comparison.candidate = summarize(candidate);
    comparison.difference = summarize(difference, config.bootstrap, index);
    result.push_back(std::move(comparison));
  }
  return result;
}

// ---- serialization -----------------------------------------------------------------

Json experiment_json(const ExperimentConfig& config) {
  auto seeds = Json::array();
  for (const auto seed : config.seeds) {
    seeds.push_back(seed);
  }
  auto variants = Json::array();
  for (const auto& variant : config.variants) {
    auto overrides = Json::array();
    for (const auto& item : variant.overrides) {
      overrides.push_back(
          {{"agent_id", item.agent.value()}, {"policy", codec::policy(item.policy)}});
    }
    variants.push_back({{"name", variant.name}, {"policy_overrides", overrides}});
  }
  return {
      {"schema_version", kExperimentSchema},
      {"experiment_id", config.experiment_id},
      {"base_config", codec::config(config.base)},
      {"horizon_ns", config.horizon ? Json(config.horizon->value()) : Json(nullptr)},
      {"seeds", seeds},
      {"variants", variants},
      {
          "comparison",
          config.comparison ? Json{
                                  {"baseline", config.comparison->baseline},
                                  {"candidate", config.comparison->candidate},
                                  {"subject_agent_id", config.comparison->subject.value()},
                              }
                            : Json(nullptr),
      },
      {
          "bootstrap",
          {{"resamples", config.bootstrap.resamples}, {"seed", config.bootstrap.seed}},
      },
      {"artifacts", codec::enum_name(config.artifacts, kArtifactPolicies)},
      {
          "limits",
          {{"max_runs", config.max_runs}, {"max_output_bytes", config.max_output_bytes}},
      },
  };
}

std::vector<std::uint64_t> read_seeds(const Json& value) {
  std::vector<std::uint64_t> seeds;
  if (value.is_array()) {
    for (const auto& seed : value) {
      seeds.push_back(codec::unsigned_integer(seed));
    }
    return seeds;
  }
  // {"first": s, "count": n} is the consecutive list s, s + 1, ..., s + n - 1.
  codec::fields(value, {"first", "count"});
  const auto first = codec::unsigned_integer(value.at("first"));
  const auto total = codec::unsigned_integer(value.at("count"));
  require(total <= kMaxExperimentRuns && first <= ~std::uint64_t{0} - total,
          "seed range is too long or runs past 2^64");
  seeds.reserve(total);
  for (std::uint64_t offset = 0; offset < total; ++offset) {
    seeds.push_back(first + offset);
  }
  return seeds;
}

Json interval_json(const std::optional<Interval>& value) {
  return value ? Json{value->lower_milli, value->upper_milli} : Json(nullptr);
}

Json summary_json(const std::optional<Summary>& value) {
  if (!value) {
    return nullptr;
  }
  return {
      {"count", value->count},
      {"minimum", value->minimum},
      {"maximum", value->maximum},
      {"mean_milli", value->mean_milli},
      {"median_milli", value->median_milli},
      {
          "standard_deviation_milli",
          value->standard_deviation_milli ? Json(*value->standard_deviation_milli) : Json(nullptr),
      },
      {"mean_t_interval_milli", interval_json(value->mean_t_interval)},
      {"mean_bootstrap_interval_milli", interval_json(value->mean_bootstrap_interval)},
  };
}

Json run_json(const RunOutcome& run) {
  Json output{
      {"variant", run.variant},
      {"seed", run.seed},
      {"run_id", run.run_id},
      {"status", run.completed ? "Completed" : "Failed"},
      {"error_code", run.completed ? Json(nullptr) : Json(run.error_code)},
      {"error_message", run.completed ? Json(nullptr) : Json(run.error_message)},
      {"artifacts", run.artifacts.empty() ? Json(nullptr) : Json(run.artifacts)},
  };
  // A failed run has no digests; the keys stay so every entry has one shape.
  const auto summary = run.completed ? codec::summary(run.summary) : Json::object();
  for (const auto* key : {
           "stop_reason",
           "event_count",
           "trade_count",
           "final_time_ns",
           "config_digest",
           "commands_digest",
           "trace_digest",
           "book_digest",
           "trade_digest",
       }) {
    output.emplace(key, run.completed ? summary.at(key) : Json(nullptr));
  }
  output.emplace("accounting_digest",
                 run.completed ? Json(run.summary.accounting_digest) : Json(nullptr));
  output.emplace("metrics_digest",
                 run.completed ? Json(run.summary.metrics_digest) : Json(nullptr));
  return output;
}

Json results_json(const ExperimentConfig& config, const ExperimentResult& result) {
  auto runs = Json::array();
  for (const auto& run : result.runs) {
    runs.push_back(run_json(run));
  }
  auto counts = Json::array();
  for (const auto& item : result.counts) {
    counts.push_back(
        {{"variant", item.variant}, {"completed", item.completed}, {"failed", item.failed}});
  }
  Json comparison = nullptr;
  if (config.comparison) {
    auto metrics = Json::array();
    for (const auto& metric : result.comparisons) {
      metrics.push_back({
          {"name", metric.name},
          {"unit", metric.unit},
          {"scope", metric.scope},
          {"pairs", metric.pairs},
          {"pairs_missing", metric.pairs_missing},
          {"baseline", summary_json(metric.baseline)},
          {"candidate", summary_json(metric.candidate)},
          {"difference", summary_json(metric.difference)},
      });
    }
    comparison = {
        {"baseline", config.comparison->baseline},
        {"candidate", config.comparison->candidate},
        {"subject_agent_id", config.comparison->subject.value()},
        {"difference", "candidate_minus_baseline"},
        {"pairs_completed", result.pairs_completed},
        {"metrics", metrics},
    };
  }
  const auto canonical = experiment_json(config);
  return {
      {"schema_version", kExperimentSchema},
      {"kind", "ExperimentResults"},
      {"config", canonical},
      {"config_digest", sha256(canonical.dump())},
      {"metrics_schema_version", kMetricsSchema},
      {
          "design",
          {
              {"seeds", config.seeds.size()},
              {"variants", config.variants.size()},
              {"total_runs", result.runs.size()},
              {"pairing", "same_seed"},
          },
      },
      {
          "statistics",
          {
              {"scale", "milli"},
              {"rounding", "half_away_from_zero"},
              {"confidence_level_ppm", 950'000},
              {"t_interval", "student_t_two_sided"},
              {"bootstrap_interval", "percentile"},
              {"rng_algorithm", kRngAlgorithm},
          },
      },
      {"counts", counts},
      {"runs", runs},
      {"comparison", comparison},
  };
}

void write_file(const std::filesystem::path& path, std::string_view contents) {
  std::ofstream output{path, std::ios::binary};
  output << contents;
  output.close();
  if (!output) {
    throw RunError{"IoError", "failed writing " + path.string()};
  }
}

std::uint64_t directory_bytes(const std::filesystem::path& directory) {
  std::uint64_t total = 0;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator{directory, error}) {
    if (entry.is_regular_file(error)) {
      total += entry.file_size(error);
    }
  }
  return total;
}

void publish(const ExperimentConfig& config, const ExperimentResult& result,
             const std::filesystem::path& directory) {
  std::string lines;
  for (const auto& run : result.runs) {
    const auto* variant = find_variant(config, run.variant);
    const Json line{
        {"variant", run.variant},
        {"seed", run.seed},
        {"run_id", run.run_id},
        {"status", run.completed ? "Completed" : "Failed"},
        {"summary", run.completed ? codec::summary(run.summary) : Json(nullptr)},
        {
            "metrics",
            run.completed ? codec::metrics(variant_config(config, *variant, run.seed), run.summary)
                          : Json(nullptr),
        },
    };
    lines += line.dump() + '\n';
  }
  const auto results = experiment_results_json(config, result);
  const Json document{
      {"schema_version", kExperimentSchema},
      {"kind", "Experiment"},
      {"engine_version", version()},
      {"hash_algorithm", "sha256"},
      {
          "provenance",
          {
              {"git_revision", MARKETTWIN_GIT_REVISION},
              {"compiler", MARKETTWIN_COMPILER},
              {"build_type", MARKETTWIN_BUILD_TYPE},
          },
      },
      {"results", Json::parse(results)},
      {"results_digest", sha256(results)},
      {"artifacts", {{"runs.jsonl", sha256(lines)}}},
  };
  write_file(directory / "runs.jsonl", lines);
  write_file(directory / "experiment.json.partial", document.dump() + '\n');
  std::filesystem::rename(directory / "experiment.json.partial", directory / "experiment.json");
}

RunSummary default_run(const RunConfig& config, const std::filesystem::path& directory) {
  return Simulation{config, directory}.run();
}

void create_output(const std::filesystem::path& directory) {
  if (directory.empty()) {
    return;
  }
  if (!directory.parent_path().empty()) {
    std::filesystem::create_directories(directory.parent_path());
  }
  if (!std::filesystem::create_directory(directory)) {
    throw RunError{"OutputExists", "output directory already exists: " + directory.string()};
  }
}

// Runs the cells of one experiment, keeping the running total of bytes written.
class Cells {
public:
  Cells(const ExperimentConfig& config, const std::filesystem::path& output, RunFunction execute)
      : config_(&config), output_(&output), execute_(std::move(execute)),
        keep_(config.artifacts == ArtifactPolicy::All) {}

  // Never throws for a failing run: the failure is the outcome.
  [[nodiscard]] RunOutcome run(const Variant& variant, std::uint64_t seed) {
    const auto cell = variant_config(*config_, variant, seed);
    RunOutcome outcome;
    outcome.variant = variant.name;
    outcome.seed = seed;
    outcome.run_id = cell.run_id;
    const auto relative =
        std::filesystem::path{"runs"} / variant.name / ("seed-" + std::to_string(seed));
    try {
      if (keep_ && written_ >= config_->max_output_bytes) {
        throw RunError{"OutputLimit", "experiment output budget is exhausted"};
      }
      outcome.summary = execute_(cell, keep_ ? *output_ / relative : std::filesystem::path{});
      outcome.completed = true;
      if (keep_) {
        outcome.artifacts = relative.generic_string();
        written_ += directory_bytes(*output_ / relative);
      }
    } catch (const RunError& error) {
      outcome.error_code = error.code();
      outcome.error_message = error.what();
    } catch (const std::exception& error) {
      outcome.error_code = "RunFailed";
      outcome.error_message = error.what();
    }
    return outcome;
  }

private:
  const ExperimentConfig* config_;
  const std::filesystem::path* output_;
  RunFunction execute_;
  bool keep_;
  std::uint64_t written_ = 0;
};

// ---- terminal report ---------------------------------------------------------------

// Thousandths as a decimal, for example -1234567 -> "-1234.567".
std::string decimal(std::int64_t milli) {
  const auto magnitude =
      milli < 0 ? ~static_cast<std::uint64_t>(milli) + 1 : static_cast<std::uint64_t>(milli);
  auto fraction = std::to_string(magnitude % 1'000);
  fraction.insert(0, 3 - fraction.size(), '0');
  return (milli < 0 ? "-" : "") + std::to_string(magnitude / 1'000) + "." + fraction;
}

std::string pad(std::string text, std::size_t width, bool left = false) {
  if (text.size() < width) {
    text.insert(left ? text.size() : 0, width - text.size(), ' ');
  }
  return text;
}

// The t interval of a paired difference, when there were enough pairs to have one.
const Interval* t_interval(const std::optional<Summary>& difference) {
  return difference && difference->mean_t_interval ? &*difference->mean_t_interval : nullptr;
}

std::string interval_text(const Interval* interval) {
  if (interval == nullptr) {
    return "n/a";
  }
  return "[" + decimal(interval->lower_milli) + ", " + decimal(interval->upper_milli) + "]";
}

std::string_view finding(const Interval* interval) {
  if (interval == nullptr) {
    return "n/a";
  }
  if (interval->lower_milli == 0 && interval->upper_milli == 0) {
    return "identical";
  }
  if (interval->lower_milli > 0) {
    return "higher";
  }
  return interval->upper_milli < 0 ? "lower" : "inconclusive";
}

} // namespace

RunConfig variant_config(const ExperimentConfig& config, const Variant& variant,
                         std::uint64_t seed) {
  auto run = config.base;
  run.seed = seed;
  run.run_id = config.experiment_id + "-" + variant.name + "-s" + std::to_string(seed);
  if (config.horizon) {
    run.limits.horizon = *config.horizon;
  }
  if (variant.overrides.empty()) {
    return run;
  }
  if (!run.scenario) {
    throw RunError{"InvalidConfig", "policy overrides need a base config with a scenario"};
  }
  auto& agents = run.scenario->agents;
  for (const auto& item : variant.overrides) {
    const auto found = std::ranges::find(agents, item.agent, &AgentConfig::id);
    require(found != agents.end(),
            "variant " + variant.name + " overrides an agent the base scenario does not have");
    found->policy = item.policy;
  }
  return run;
}

void validate_experiment(const ExperimentConfig& config) {
  require(valid_name(config.experiment_id),
          "experiment_id must be 1-48 ASCII letters, digits, '_' or '-'");
  require(!config.seeds.empty() && !config.variants.empty(),
          "an experiment needs at least one seed and one variant");
  require(config.max_runs >= 1 && config.max_runs <= kMaxExperimentRuns &&
              config.seeds.size() <= config.max_runs / config.variants.size(),
          "seeds times variants exceeds limits.max_runs (at most 100,000)");
  require(std::set<std::uint64_t>(config.seeds.begin(), config.seeds.end()).size() ==
              config.seeds.size(),
          "experiment seeds must be unique");
  require(config.bootstrap.resamples == 0 || (config.bootstrap.resamples >= 100 &&
                                              config.bootstrap.resamples <= kMaxBootstrapResamples),
          "bootstrap resamples must be 0 (off) or 100 to 1,000,000");
  require(config.artifacts == ArtifactPolicy::None ||
              (config.artifacts == ArtifactPolicy::All && config.max_output_bytes > 0),
          "writing run artifacts needs a positive limits.max_output_bytes");
  std::set<std::string> names;
  for (const auto& variant : config.variants) {
    require(valid_name(variant.name), "variant names must be 1-48 ASCII letters, digits, '_', '-'");
    require(names.insert(variant.name).second, "variant names must be unique");
    std::set<AgentId> overridden;
    for (const auto& item : variant.overrides) {
      require(overridden.insert(item.agent).second,
              "variant " + variant.name + " overrides one agent twice");
    }
    // Every seed shares this shape, so one cell proves the whole variant is runnable.
    validate_config(variant_config(config, variant, config.seeds.front()));
  }
  if (config.comparison) {
    const auto& comparison = *config.comparison;
    require(comparison.baseline != comparison.candidate &&
                find_variant(config, comparison.baseline) != nullptr &&
                find_variant(config, comparison.candidate) != nullptr,
            "comparison must name two different variants of this experiment");
    require(std::ranges::find(config.base.exchange.agents, comparison.subject) !=
                config.base.exchange.agents.end(),
            "comparison subject is not an agent of the base config");
    require(config.seeds.size() <= kMaxStatisticValues, "too many seeds for paired statistics");
  }
}

ExperimentConfig parse_experiment(std::string_view text,
                                  const std::filesystem::path& base_directory) {
  if (text.size() > codec::kMaxConfigBytes) {
    throw RunError{"ResourceLimit", "experiment config exceeds 16 MiB"};
  }
  const auto value = codec::parse(text);
  if (!value.is_object() || !value.contains("schema_version") ||
      codec::unsigned_integer(value.at("schema_version")) != kExperimentSchema) {
    throw RunError{"UnsupportedSchema", "expected experiment schema version 1"};
  }
  codec::fields(value, {
                           "schema_version",
                           "experiment_id",
                           "base_config",
                           "horizon_ns",
                           "seeds",
                           "variants",
                           "comparison",
                           "bootstrap",
                           "artifacts",
                           "limits",
                       });
  ExperimentConfig config;
  config.experiment_id = codec::text(value.at("experiment_id"));
  const auto& base = value.at("base_config");
  config.base = base.is_string() ? load_config(base_directory / base.get<std::string>())
                                 : codec::read_config(base);
  if (const auto& horizon = value.at("horizon_ns"); !horizon.is_null()) {
    config.horizon = SimTime{codec::unsigned_integer(horizon)};
  }
  config.seeds = read_seeds(value.at("seeds"));
  if (!value.at("variants").is_array()) {
    codec::invalid("variants must be an array");
  }
  for (const auto& item : value.at("variants")) {
    codec::fields(item, {"name", "policy_overrides"});
    Variant variant{.name = codec::text(item.at("name")), .overrides = {}};
    if (!item.at("policy_overrides").is_array()) {
      codec::invalid("policy_overrides must be an array");
    }
    for (const auto& entry : item.at("policy_overrides")) {
      codec::fields(entry, {"agent_id", "policy"});
      variant.overrides.push_back({
          .agent = AgentId{codec::unsigned_integer(entry.at("agent_id"))},
          .policy = codec::read_policy(entry.at("policy")),
      });
    }
    config.variants.push_back(std::move(variant));
  }
  if (const auto& comparison = value.at("comparison"); !comparison.is_null()) {
    codec::fields(comparison, {"baseline", "candidate", "subject_agent_id"});
    config.comparison = ComparisonConfig{
        .baseline = codec::text(comparison.at("baseline")),
        .candidate = codec::text(comparison.at("candidate")),
        .subject = AgentId{codec::unsigned_integer(comparison.at("subject_agent_id"))},
    };
  }
  codec::fields(value.at("bootstrap"), {"resamples", "seed"});
  config.bootstrap = {
      .resamples = codec::unsigned_integer(value.at("bootstrap").at("resamples")),
      .seed = codec::unsigned_integer(value.at("bootstrap").at("seed")),
  };
  config.artifacts = codec::read_enum<ArtifactPolicy>(value.at("artifacts"), kArtifactPolicies);
  codec::fields(value.at("limits"), {"max_runs", "max_output_bytes"});
  config.max_runs = codec::unsigned_integer(value.at("limits").at("max_runs"));
  config.max_output_bytes = codec::unsigned_integer(value.at("limits").at("max_output_bytes"));
  validate_experiment(config);
  return config;
}

ExperimentConfig load_experiment(const std::filesystem::path& path) {
  return parse_experiment(codec::read_file(path, codec::kMaxConfigBytes), path.parent_path());
}

std::string canonical_experiment(const ExperimentConfig& config) {
  validate_experiment(config);
  return experiment_json(config).dump();
}

ExperimentResult run_experiment(const ExperimentConfig& config,
                                const std::filesystem::path& output_directory,
                                const RunFunction& run) {
  validate_experiment(config);
  require(config.artifacts == ArtifactPolicy::None || !output_directory.empty(),
          "writing run artifacts needs an experiment output directory");
  try {
    create_output(output_directory);
    Cells cells{config, output_directory, run ? run : RunFunction{default_run}};
    ExperimentResult result;
    for (const auto& variant : config.variants) {
      result.counts.push_back({.variant = variant.name, .completed = 0, .failed = 0});
    }
    for (const auto seed : config.seeds) {
      for (std::size_t index = 0; index < config.variants.size(); ++index) {
        auto outcome = cells.run(config.variants.at(index), seed);
        auto& counts = result.counts.at(index);
        ++(outcome.completed ? counts.completed : counts.failed);
        result.runs.push_back(std::move(outcome));
      }
    }
    result.comparisons = compare(config, result.runs, result.pairs_completed);
    if (!output_directory.empty()) {
      publish(config, result, output_directory);
    }
    return result;
  } catch (const std::filesystem::filesystem_error& error) {
    throw RunError{"IoError", error.what()};
  }
}

std::string experiment_results_json(const ExperimentConfig& config,
                                    const ExperimentResult& result) {
  return results_json(config, result).dump();
}

std::string experiment_report(const ExperimentConfig& config, const ExperimentResult& result) {
  std::string text = "MarketTwin experiment " + config.experiment_id + "\n";
  text += "  engine        " + std::string{version()} + ", experiment schema " +
          std::to_string(kExperimentSchema) + ", metrics schema " + std::to_string(kMetricsSchema) +
          "\n";
  text += "  base config   " + config.base.run_id + " (sha256 " +
          sha256(canonical_config(config.base)).substr(0, 12) + ")\n";
  text += "  design        " + std::to_string(config.seeds.size()) + " seeds x " +
          std::to_string(config.variants.size()) +
          " variants = " + std::to_string(result.runs.size()) + " runs, paired by seed\n";
  for (const auto& item : result.counts) {
    text += "  runs          " + pad(item.variant, 24, true) + " " +
            std::to_string(item.completed) + " completed, " + std::to_string(item.failed) +
            " failed\n";
  }
  for (const auto& run : result.runs) {
    if (!run.completed) {
      text +=
          "  failed        " + run.run_id + ": " + run.error_code + ": " + run.error_message + "\n";
    }
  }
  if (!config.comparison) {
    return text;
  }
  const auto& comparison = *config.comparison;
  text += "  comparison    " + comparison.candidate + " minus " + comparison.baseline +
          " for agent " + std::to_string(comparison.subject.value()) + ", " +
          std::to_string(result.pairs_completed) + " paired seeds\n\n";
  text += "  " + pad("metric", 36, true) + pad("unit", 12, true) + pad("pairs", 6) +
          pad("baseline mean", 16) + pad("candidate mean", 16) + pad("mean diff", 15) +
          pad("median diff", 15) + pad("sd diff", 15) + "  " +
          pad("95% t interval of mean diff", 32, true) + "candidate is\n";
  for (const auto& metric : result.comparisons) {
    const auto mean = [](const std::optional<Summary>& summary) {
      return summary ? decimal(summary->mean_milli) : std::string{"n/a"};
    };
    const auto& difference = metric.difference;
    const auto* interval = t_interval(difference);
    text += "  " + pad(metric.scope + "." + metric.name, 36, true) + pad(metric.unit, 12, true) +
            pad(std::to_string(metric.pairs), 6) + pad(mean(metric.baseline), 16) +
            pad(mean(metric.candidate), 16) + pad(mean(difference), 15) +
            pad(difference ? decimal(difference->median_milli) : "n/a", 15) +
            pad(difference && difference->standard_deviation_milli
                    ? decimal(*difference->standard_deviation_milli)
                    : "n/a",
                15) +
            "  " + pad(interval_text(interval), 32, true) + std::string{finding(interval)} + "\n";
  }
  text +=
      "\n  Differences are candidate minus baseline on the same seed. Money is in minor units and\n"
      "  every statistic is shown to three decimals of its unit. 'higher' or 'lower' means the\n"
      "  95% Student t interval of the mean paired difference excludes zero; 'inconclusive'\n"
      "  means it does not. Bootstrap intervals and medians are in experiment.json.\n"
      "  These intervals describe variation across seeds of one stylized simulated market.\n"
      "  They are not evidence that a strategy is realistic or profitable, and " +
      std::to_string(result.comparisons.size()) +
      " metrics are\n  tested with no multiplicity correction.\n";
  return text;
}

} // namespace markettwin
