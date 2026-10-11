#pragma once

#include "markettwin/simulation/simulation.hpp"

namespace markettwin {
[[nodiscard]] RunSummary write_run(const RunConfig& config, const std::filesystem::path& directory);
[[nodiscard]] RunArtifacts artifact_paths(const std::filesystem::path& directory);
} // namespace markettwin
