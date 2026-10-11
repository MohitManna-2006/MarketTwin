#pragma once

#include "markettwin/exchange/matching_engine.hpp"
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
};

void validate_config(const RunConfig& config);
[[nodiscard]] RunConfig parse_config(std::string_view text);
[[nodiscard]] RunConfig load_config(const std::filesystem::path& path);
[[nodiscard]] std::string canonical_config(const RunConfig& config);

} // namespace markettwin
