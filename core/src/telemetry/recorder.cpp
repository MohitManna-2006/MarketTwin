#include "recorder.hpp"

#include "markettwin/common/version.hpp"

namespace markettwin {
namespace {
using codec::Json;

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
  append(TraceChannel::Commands, header.dump() + '\n', {});
  header.update({{"channel", "execution"}});
  append(TraceChannel::Execution, header.dump() + '\n', {});
}

void Recorder::append(TraceChannel channel, const std::string& line, Sequence sequence) {
  if (line.size() > codec::kMaxRecordBytes || line.size() > config_.max_trace_bytes - bytes_) {
    throw RunError{"ResourceLimit", "canonical trace exceeds byte budget", sequence};
  }
  sink_.append(channel, line, sequence);
  auto& hash = channel == TraceChannel::Commands ? commands_hash_ : trace_hash_;
  hash.update(line);
  bytes_ += line.size();
}

// Writes the envelope directly in canonical form: sorted keys, compact, integers only.
// The payload is already canonical, and run IDs and kinds never need escaping, so this
// produces the bytes a JSON library would while serializing the payload exactly once.
void Recorder::write(TraceChannel channel, const Event& event, Sequence sequence,
                     std::string_view kind, const std::string& payload, AgentId actor,
                     OrderId correlation, Sequence causation) {
  const auto optional_id = [this](std::string_view key, std::uint64_t value) {
    line_.append(key);
    if (value == 0) {
      line_.append("null");
    } else {
      line_.append(std::to_string(value));
    }
  };
  line_.clear();
  optional_id(R"({"actor_id":)", actor.value());
  optional_id(R"(,"causation_id":)", causation.value());
  optional_id(R"(,"correlation_id":)", correlation.value());
  line_.append(R"(,"kind":")").append(kind);
  line_.append(R"(","payload":)").append(payload);
  line_.append(R"(,"payload_digest":")").append(sha256(payload));
  line_.append(R"(","run_id":")").append(config_.run_id);
  optional_id(R"(","scheduler_sequence":)", event.scheduler_sequence.value());
  line_.append(R"(,"schema_version":1,"sequence":)").append(std::to_string(sequence.value()));
  line_.append(R"(,"timestamp_ns":)").append(std::to_string(event.timestamp.value()));
  line_.append("}\n");
  append(channel, line_, sequence);
}

Sequence Recorder::command(const Event& event, const Command& command) {
  const auto sequence = sequence_.next();
  AgentId actor;
  OrderId correlation;
  if (const auto* order = std::get_if<Order>(&command)) {
    actor = order->owner;
    correlation = order->id;
  } else if (const auto* cancel = std::get_if<CancelCommand>(&command)) {
    actor = cancel->owner;
    correlation = cancel->order_id;
  }
  write(TraceChannel::Commands, event, sequence, command_kind(command),
        codec::command(command).dump(), actor, correlation, {});
  return sequence;
}

void Recorder::outcome(const Event& cause, Sequence causation, std::string_view kind,
                       const std::string& payload, AgentId actor, OrderId correlation) {
  write(TraceChannel::Execution, cause, sequence_.next(), kind, payload, actor, correlation,
        causation);
}

void Recorder::trade(const Event& cause, Sequence causation, const Trade& trade) {
  const auto payload = codec::trade(trade);
  trades_hash_.update(payload);
  trades_hash_.update("\n");
  write(TraceChannel::Execution, cause, sequence_.next(), "Trade", payload, trade.taker_agent_id,
        trade.aggressor_order_id, causation);
}

void Recorder::finish(RunSummary& summary) {
  summary.config_digest = config_digest_;
  summary.trade_digest = trades_hash_.digest();
  const Event terminal_event{
      .timestamp = summary.final_time,
      .scheduler_sequence = {},
      .payload = StopCommand{},
  };
  outcome(terminal_event, {}, "RunCompleted", codec::terminal(summary).dump());
  summary.commands_digest = commands_hash_.digest();
  summary.trace_digest = trace_hash_.digest();
}

} // namespace markettwin
