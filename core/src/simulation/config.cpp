#include "markettwin/simulation/config.hpp"

#include "../telemetry/codec.hpp"

#include <algorithm>

namespace markettwin {
namespace {
void validate_command(const ScheduledCommand& item, const RunConfig& config) {
  if (item.command.valueless_by_exception()) {
    throw RunError{"InvalidPayload", "empty command variant"};
  }
  if (const auto* order = std::get_if<Order>(&item.command)) {
    // Domain-invalid prices, owners, quantities and IDs are exchange commands and
    // produce logged rejections. Malformed types/arrival metadata are config errors.
    static_cast<void>(codec::command(item.command));
    if (order->arrival_time != SimTime{} || order->arrival_sequence != Sequence{} ||
        order->remaining_quantity != order->original_quantity) {
      throw RunError{"InvalidConfig", "kernel owns arrival metadata; new quantities must agree"};
    }
  }
  if (const auto* sample = std::get_if<SnapshotCommand>(&item.command)) {
    if (sample->depth == 0 || sample->depth > config.exchange.max_snapshot_depth) {
      throw RunError{"InvalidConfig", "snapshot depth outside configured bounds"};
    }
  }
}
} // namespace

void validate_config(const RunConfig& config) {
  if (config.run_id.empty() || config.run_id.size() > 128 ||
      !std::ranges::all_of(config.run_id, [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
               ch == '_' || ch == '-';
      })) {
    throw RunError{"InvalidConfig", "run_id must contain 1-128 ASCII letters, digits, '_' or '-'"};
  }
  if (config.tick_size_minor == 0 || config.max_trace_bytes == 0) {
    throw RunError{"InvalidConfig", "tick scale and trace byte limit must be positive"};
  }
  const Scheduler scheduler{config.limits};
  if (config.commands.size() > config.limits.max_queue) {
    throw RunError{"QueueCapacity", "initial script exceeds queue capacity"};
  }
  try {
    const MatchingEngine exchange{config.exchange};
  } catch (const ExchangeError& error) {
    throw RunError{"InvalidConfig", error.what()};
  }
  for (const auto& item : config.commands) {
    validate_command(item, config);
  }
}

RunConfig parse_config(std::string_view text) {
  if (text.size() > codec::kMaxConfigBytes) {
    throw RunError{"ResourceLimit", "config exceeds 16 MiB"};
  }
  return codec::read_config(codec::parse(text));
}

RunConfig load_config(const std::filesystem::path& path) {
  return parse_config(codec::read_file(path, codec::kMaxConfigBytes));
}

std::string canonical_config(const RunConfig& config) {
  validate_config(config);
  auto result = codec::config(config).dump();
  if (result.size() > codec::kMaxConfigBytes) {
    throw RunError{"ResourceLimit", "config exceeds 16 MiB"};
  }
  return result;
}

} // namespace markettwin
