#pragma once

#include <cstdint>

namespace markettwin {

// Phase 0 run boundary. Later phases add scenario fields without moving this type.
struct RunConfig {
  std::uint64_t seed = 0;
};

// Underlying type stays the language default until event serialization chooses a width.
enum class RunStatus { Completed };

struct RunSummary {
  RunStatus status = RunStatus::Completed;
  std::uint64_t event_count = 0;
  std::uint64_t seed = 0;
};

// Reserved for artifact paths once telemetry export exists.
struct RunArtifacts {};

// Deterministic no-op. This type does not match orders, schedule events, or trade.
class Simulation {
public:
  explicit Simulation(RunConfig config);

  // Non-const because later phases mutate kernel state here.
  [[nodiscard]] RunSummary run();
  [[nodiscard]] RunArtifacts artifacts() const noexcept;

private:
  RunConfig config_;
};

} // namespace markettwin
