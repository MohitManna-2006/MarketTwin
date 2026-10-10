#include "markettwin/simulation/simulation.hpp"

namespace markettwin {

Simulation::Simulation(RunConfig config) : config_(config) {}

// NOLINTNEXTLINE(readability-make-member-function-const)
RunSummary Simulation::run() {
  RunSummary summary;
  summary.status = RunStatus::Completed;
  summary.event_count = 0;
  summary.seed = config_.seed;
  return summary;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
RunArtifacts Simulation::artifacts() const noexcept {
  return {};
}

} // namespace markettwin
