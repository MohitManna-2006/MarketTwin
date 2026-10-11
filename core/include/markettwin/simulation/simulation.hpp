#pragma once

#include "markettwin/simulation/config.hpp"

namespace markettwin {

enum class RunStatus { Completed };

struct RunSummary {
  RunStatus status = RunStatus::Completed;
  StopReason stop_reason = StopReason::QueueEmpty;
  std::uint64_t event_count = 0;
  std::uint64_t trade_count = 0;
  std::uint64_t seed = 0;
  SimTime final_time;
  std::size_t resting_order_count = 0;
  std::string config_digest;
  std::string book_digest;
  std::string trade_digest;
  std::string commands_digest;
  std::string trace_digest;
  bool operator==(const RunSummary&) const = default;
};

struct RunArtifacts {
  std::filesystem::path directory;
  std::filesystem::path config;
  std::filesystem::path commands;
  std::filesystem::path trace;
  std::filesystem::path manifest;
  bool operator==(const RunArtifacts&) const = default;
};

class Simulation {
public:
  explicit Simulation(RunConfig config, std::filesystem::path output_directory = {});
  [[nodiscard]] RunSummary run();
  [[nodiscard]] RunArtifacts artifacts() const;

private:
  RunConfig config_;
  std::filesystem::path output_directory_;
  RunArtifacts artifacts_;
};

} // namespace markettwin
