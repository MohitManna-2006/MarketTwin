#ifndef MARKETTWIN_TELEMETRY_FORK_HPP
#define MARKETTWIN_TELEMETRY_FORK_HPP

#include "markettwin/simulation/config.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace markettwin {

// The outcome of checking a fork against its parent (ADR 0012). On success `report` is
// the fork_report.json document, without its final newline.
struct ForkVerification {
  bool verified = false;
  std::string code;
  std::string message;
  std::string report;
};

// Replays both runs, checks the fork's last stage names the parent's config, checks that
// both traces agree byte for byte with run IDs normalized up to the fork's ForkPoint, and
// finds the first divergence after it. Failures are reported, never thrown.
[[nodiscard]] ForkVerification verify_fork(const std::filesystem::path& parent,
                                           const std::filesystem::path& fork);
[[nodiscard]] std::string fork_verification_json(const ForkVerification& result);

// A directive as the Decision records and fork stages spell it.
[[nodiscard]] Directive parse_directive(std::string_view json);
[[nodiscard]] std::string directive_json(const Directive& directive);
// What a decision source outside the core receives and returns (ADR 0012):
//   request  {"agent_id", "at_ns", "index", "mandate", "observation", "current"}
//   response {"directive": {...} | null, "model", "input_tokens", "output_tokens",
//             "error_code": string | null}
// A malformed response is a failed decision with code SourceResponseInvalid.
[[nodiscard]] std::string decision_request_json(const DecisionRequest& request);
[[nodiscard]] DecisionResponse parse_decision_response(std::string_view json);
// Every Decision record of a finished run, in trace order.
[[nodiscard]] std::vector<RecordedDecision> read_decisions(const std::filesystem::path& run);
// A stage from a request that may leave out every field but at_ns; the parent fields and
// recorded decisions are filled in by fork_of.
[[nodiscard]] ForkStage parse_stage_request(std::string_view json);
// The config of a fork of a finished run: its config with `stage` appended, the stage's
// parent ID, parent digest, and recorded decisions filled in, under a new run ID.
// Throws RunError{"InvalidConfig"} if the result is not a valid fork.
[[nodiscard]] RunConfig fork_of(const std::filesystem::path& parent, ForkStage stage,
                                std::string run_id);

} // namespace markettwin
#endif // MARKETTWIN_TELEMETRY_FORK_HPP
