#include "recorder.hpp"

#include "markettwin/common/version.hpp"

namespace markettwin {
namespace {
using codec::Json;

Json optional_id(std::uint64_t value) {
  return value == 0 ? Json(nullptr) : Json(value);
}

std::string_view command_kind(const Command& command) {
  if (std::holds_alternative<Order>(command)) {
    return "OrderArrival";
  }
  if (std::holds_alternative<CancelCommand>(command)) {
    return "CancelArrival";
  }
  if (std::holds_alternative<SnapshotCommand>(command)) {
    return "SnapshotRequest";
  }
  return "SimulationStop";
}
} // namespace

Recorder::Recorder(const RunConfig& config, RecordSink& sink)
    : config_(config), sink_(sink), config_digest_(sha256(canonical_config(config))) {
  Json header{
      {"schema_version", 1},
      {"kind", "Header"},
      {"run_id", config.run_id},
      {"engine_version", version()},
      {"config_digest", config_digest_},
      {"hash_algorithm", "sha256"},
      {
          "units",
          {
              {"price", "ticks"},
              {"quantity", "shares"},
              {"time", "ns"},
              {"tick_size_minor", config.tick_size_minor},
          },
      },
  };
  header.update({{"channel", "commands"}});
  write(TraceChannel::Commands, header, {});
  header.update({{"channel", "execution"}});
  write(TraceChannel::Execution, header, {});
}

void Recorder::write(TraceChannel channel, const Json& record, Sequence sequence) {
  const auto line = record.dump() + '\n';
  if (line.size() > codec::kMaxRecordBytes || line.size() > config_.max_trace_bytes - bytes_) {
    throw RunError{"ResourceLimit", "canonical trace exceeds byte budget", sequence};
  }
  sink_.append(channel, line, sequence);
  auto& hash = channel == TraceChannel::Commands ? commands_hash_ : trace_hash_;
  hash.update(line);
  bytes_ += line.size();
}

Json Recorder::envelope(const Event& event, Sequence sequence, std::string_view kind,
                        const Json& payload, AgentId actor, OrderId correlation,
                        Sequence causation) const {
  return {
      {"schema_version", 1},
      {"run_id", config_.run_id},
      {"sequence", sequence.value()},
      {"timestamp_ns", event.timestamp.value()},
      {"kind", kind},
      {"scheduler_sequence", optional_id(event.scheduler_sequence.value())},
      {"actor_id", optional_id(actor.value())},
      {"correlation_id", optional_id(correlation.value())},
      {"causation_id", optional_id(causation.value())},
      {"payload", payload},
      {"payload_digest", sha256(payload.dump())},
  };
}

Sequence Recorder::command(const Event& event) {
  const auto sequence = sequence_.next();
  AgentId actor;
  OrderId correlation;
  if (const auto* order = std::get_if<Order>(&event.command)) {
    actor = order->owner;
    correlation = order->id;
  } else if (const auto* cancel = std::get_if<CancelCommand>(&event.command)) {
    actor = cancel->owner;
    correlation = cancel->order_id;
  }
  write(TraceChannel::Commands,
        envelope(event, sequence, command_kind(event.command), codec::command(event.command), actor,
                 correlation, {}),
        sequence);
  return sequence;
}

void Recorder::outcome(const Event& cause, Sequence causation, std::string_view kind,
                       const Json& payload, AgentId actor, OrderId correlation) {
  const auto sequence = sequence_.next();
  write(TraceChannel::Execution,
        envelope(cause, sequence, kind, payload, actor, correlation, causation), sequence);
}

void Recorder::record_trade(const Trade& trade) {
  trades_hash_.update(codec::trade(trade).dump() + '\n');
}

void Recorder::finish(RunSummary& summary) {
  summary.config_digest = config_digest_;
  summary.trade_digest = trades_hash_.digest();
  const Event terminal_event{
      .timestamp = summary.final_time,
      .scheduler_sequence = {},
      .command = StopCommand{},
  };
  outcome(terminal_event, {}, "RunCompleted", codec::terminal(summary));
  summary.commands_digest = commands_hash_.digest();
  summary.trace_digest = trace_hash_.digest();
}

} // namespace markettwin
