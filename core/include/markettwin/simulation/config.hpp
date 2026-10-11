#ifndef MARKETTWIN_SIMULATION_CONFIG_HPP
#define MARKETTWIN_SIMULATION_CONFIG_HPP

#include "markettwin/exchange/matching_engine.hpp"
#include "markettwin/simulation/scenario.hpp"
#include "markettwin/simulation/scheduler.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace markettwin {

struct RunConfig {
  std::uint64_t seed = 0;
  std::string run_id = "run";
  ExchangeConfig exchange{.symbol = SymbolId{1}, .agents = {AgentId{1}, AgentId{2}}};
  SchedulerLimits limits;
  std::uint64_t tick_size_minor = 1;
  std::uint64_t max_trace_bytes = 268'435'456;
  std::vector<ScheduledCommand> commands;
  // Absent for a purely scripted run. Present, it adds agents, a fundamental process,
  // and shocks, and the config serializes as schema version 2.
  std::optional<ScenarioConfig> scenario;
};

void validate_config(const RunConfig& config);
[[nodiscard]] RunConfig parse_config(std::string_view text);
[[nodiscard]] RunConfig load_config(const std::filesystem::path& path);
[[nodiscard]] std::string canonical_config(const RunConfig& config);

} // namespace markettwin
#endif // MARKETTWIN_SIMULATION_CONFIG_HPP
