#ifndef MARKETTWIN_TELEMETRY_REPLAY_HPP
#define MARKETTWIN_TELEMETRY_REPLAY_HPP

#include "markettwin/simulation/simulation.hpp"

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
[[nodiscard]] std::string replay_result_json(const ReplayResult& result);
[[nodiscard]] std::string summary_json(const RunSummary& summary);
// The metrics.json document for a finished run of this config, without the final newline.
[[nodiscard]] std::string metrics_json(const RunConfig& config, const RunSummary& summary);

} // namespace markettwin
#endif // MARKETTWIN_TELEMETRY_REPLAY_HPP
