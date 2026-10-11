#ifndef MARKETTWIN_SIMULATION_SIMULATION_HPP
#define MARKETTWIN_SIMULATION_SIMULATION_HPP

#include "markettwin/simulation/config.hpp"

namespace markettwin {

enum class RunStatus { Completed };

// What one agent did. Counts and net_position are taken at the exchange, not from the
// agent's delayed view, so net positions across all trading parties sum to zero.
struct AgentActivity {
  AgentId id;
  std::string policy;
  std::uint64_t wakeups = 0;
  std::uint64_t orders_sent = 0;
  std::uint64_t cancels_sent = 0;
  std::uint64_t intents_dropped = 0; // refused by the agent's own limits
  std::uint64_t fills = 0;
  Quantity filled_quantity;
  std::int64_t net_position = 0;
  bool operator==(const AgentActivity&) const = default;
};

struct ScenarioSummary {
  std::int64_t fundamental_milliticks = 0;
  std::uint64_t fundamental_updates = 0;
  std::uint64_t market_data_updates = 0;
  std::uint64_t shock_transitions = 0;
  std::string rng_digest; // every stream's final state
  std::vector<AgentActivity> agents;
  bool operator==(const ScenarioSummary&) const = default;
};

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
  std::optional<ScenarioSummary> scenario;
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
#endif // MARKETTWIN_SIMULATION_SIMULATION_HPP
