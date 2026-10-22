#ifndef MARKETTWIN_TELEMETRY_REPLAY_HPP
#define MARKETTWIN_TELEMETRY_REPLAY_HPP

#include "markettwin/simulation/simulation.hpp"

#include <memory>

namespace markettwin {

struct ReplayResult {
  bool verified = false;
  std::string code;
  std::string message;
  Sequence sequence;
  std::string expected;
  std::string actual;
  std::string previous_command;
  std::string previous_output;
  RunSummary summary;
};

[[nodiscard]] ReplayResult verify_replay(const std::filesystem::path& directory);
// Answers AI desk decisions from a finished run's Decision records (ADR 0012), so the
// same config runs again exactly without asking a model.
[[nodiscard]] std::unique_ptr<DecisionSource> recorded_decisions(
    const std::filesystem::path& run_directory);
[[nodiscard]] std::string replay_result_json(const ReplayResult& result);
[[nodiscard]] std::string summary_json(const RunSummary& summary);
// The metrics.json document for a finished run of this config, without the final newline.
[[nodiscard]] std::string metrics_json(const RunConfig& config, const RunSummary& summary);

} // namespace markettwin
#endif // MARKETTWIN_TELEMETRY_REPLAY_HPP
