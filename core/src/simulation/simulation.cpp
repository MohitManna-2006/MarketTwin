#include "markettwin/simulation/simulation.hpp"

#include "markettwin/simulation/kernel.hpp"

#include "../telemetry/artifacts.hpp"

#include <utility>

namespace markettwin {

Simulation::Simulation(RunConfig config, std::filesystem::path output_directory)
    : config_(std::move(config)), output_directory_(std::move(output_directory)) {
  validate_config(config_);
}

RunSummary Simulation::run() {
  artifacts_ = {};
  if (output_directory_.empty()) {
    NullSink sink;
    return run_kernel(config_, sink);
  }
  auto summary = write_run(config_, output_directory_);
  artifacts_ = artifact_paths(output_directory_);
  return summary;
}

RunArtifacts Simulation::artifacts() const {
  return artifacts_;
}

} // namespace markettwin
