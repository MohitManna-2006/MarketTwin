#ifndef MARKETTWIN_EXPERIMENT_EXPERIMENT_HPP
#define MARKETTWIN_EXPERIMENT_EXPERIMENT_HPP

#include "markettwin/experiment/statistics.hpp"
#include "markettwin/simulation/simulation.hpp"

#include <functional>

namespace markettwin {

inline constexpr std::uint64_t kExperimentSchema = 1;
inline constexpr std::uint64_t kMaxExperimentRuns = 100'000;

// Replaces one agent's policy and nothing else. The agent keeps its ID, and with it its
// random streams, wakeup schedule, latency, and limits, so a variant differs from the
// base scenario only in how that participant decides.
struct PolicyOverride {
  AgentId agent;
  PolicyConfig policy;
  bool operator==(const PolicyOverride&) const = default;
};

struct Variant {
  std::string name;
  std::vector<PolicyOverride> overrides;
  bool operator==(const Variant&) const = default;
};

// Candidate minus baseline for one agent, over the seeds both variants completed.
struct ComparisonConfig {
  std::string baseline;
  std::string candidate;
  AgentId subject;
  bool operator==(const ComparisonConfig&) const = default;
};

// None keeps every run in memory and records its digests. All writes each run's full
// directory under runs/<variant>/seed-<seed>, where `markettwin replay` can verify it.
enum class ArtifactPolicy { None, All };

struct ExperimentConfig {
  std::string experiment_id = "experiment";
  RunConfig base;
  std::optional<SimTime> horizon;   // replaces the base horizon when set
  std::vector<std::uint64_t> seeds; // unique; runs execute in this order
  std::vector<Variant> variants;    // every variant runs every seed
  std::optional<ComparisonConfig> comparison;
  BootstrapConfig bootstrap;
  ArtifactPolicy artifacts = ArtifactPolicy::None;
  std::uint64_t max_runs = 1'000;     // seeds * variants may not exceed this
  std::uint64_t max_output_bytes = 0; // budget for run directories; required for All
};

// Throws RunError{"InvalidConfig"} for an empty or oversized design, duplicate seeds or
// names, an override or comparison naming an unknown agent or variant, or a variant whose
// run config is itself invalid.
void validate_experiment(const ExperimentConfig& config);
// base_directory resolves a base_config given as a relative path.
[[nodiscard]] ExperimentConfig parse_experiment(std::string_view text,
                                                const std::filesystem::path& base_directory);
[[nodiscard]] ExperimentConfig load_experiment(const std::filesystem::path& path);
// Self-contained canonical form: explicit seeds and the base config embedded.
[[nodiscard]] std::string canonical_experiment(const ExperimentConfig& config);
// The exact run config of one cell of the design. Its run_id is
// "<experiment_id>-<variant>-s<seed>", so the cell has one config digest wherever it runs.
[[nodiscard]] RunConfig variant_config(const ExperimentConfig& config, const Variant& variant,
                                       std::uint64_t seed);

struct RunOutcome {
  std::string variant;
  std::uint64_t seed = 0;
  std::string run_id;
  bool completed = false;
  std::string error_code; // set when the run failed or was not attempted
  std::string error_message;
  std::string artifacts; // run directory relative to the experiment, or empty
  RunSummary summary;    // meaningful only when completed
  bool operator==(const RunOutcome&) const = default;
};

// One metric compared across paired seeds. A pair counts only when both runs completed
// and both have the metric; the others are counted, never filled in.
struct MetricComparison {
  std::string name;
  std::string unit;  // statistics are in thousandths of this unit
  std::string scope; // "subject" for the compared agent, "market" for the whole book
  std::uint64_t pairs = 0;
  std::uint64_t pairs_missing = 0; // both runs completed but the metric was undefined
  std::optional<Summary> baseline;
  std::optional<Summary> candidate;
  std::optional<Summary> difference; // candidate - baseline, seed by seed
  bool operator==(const MetricComparison&) const = default;
};

struct VariantCount {
  std::string variant;
  std::uint64_t completed = 0;
  std::uint64_t failed = 0;
  bool operator==(const VariantCount&) const = default;
};

struct ExperimentResult {
  std::vector<RunOutcome> runs; // seed-major: for each seed, every variant in order
  std::vector<VariantCount> counts;
  std::uint64_t pairs_completed = 0; // seeds where baseline and candidate both completed
  std::vector<MetricComparison> comparisons;
  bool operator==(const ExperimentResult&) const = default;
};

// Runs one cell: in memory when the directory is empty, else writing a run directory.
using RunFunction =
    std::function<RunSummary(const RunConfig& config, const std::filesystem::path& directory)>;

// Executes every cell in order on the calling thread. A cell that throws is recorded as
// failed and the experiment continues. With an output directory (which must not exist)
// it writes experiment.json, runs.jsonl, and any run directories; experiment.json is
// published last, so its presence marks a finished experiment.
[[nodiscard]] ExperimentResult run_experiment(const ExperimentConfig& config,
                                              const std::filesystem::path& output_directory = {},
                                              const RunFunction& run = {});

// The deterministic results document: config, per-run status and digests, and the
// comparisons. Identical on every toolchain; build provenance is kept outside it.
[[nodiscard]] std::string experiment_results_json(const ExperimentConfig& config,
                                                  const ExperimentResult& result);
// Fixed-width text for a terminal.
[[nodiscard]] std::string experiment_report(const ExperimentConfig& config,
                                            const ExperimentResult& result);

} // namespace markettwin
#endif // MARKETTWIN_EXPERIMENT_EXPERIMENT_HPP
