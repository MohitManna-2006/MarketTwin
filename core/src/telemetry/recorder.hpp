#pragma once

#include "markettwin/common/hash.hpp"
#include "markettwin/telemetry/event_log.hpp"

#include "codec.hpp"

namespace markettwin {

class Recorder {
public:
  Recorder(const RunConfig& config, RecordSink& sink);
  // Appends an exchange input to the command log and returns its trace sequence.
  [[nodiscard]] Sequence command(const Event& event, const Command& command);
  // Appends a result to the execution trace. A zero causation means no command caused it.
  // The payload must already be canonical JSON text.
  void outcome(const Event& cause, Sequence causation, std::string_view kind,
               const std::string& payload, AgentId actor = {}, OrderId correlation = {});
  // A trade outcome that also extends the trade digest.
  void trade(const Event& cause, Sequence causation, const Trade& trade);
  void finish(RunSummary& summary);
  [[nodiscard]] const std::string& config_digest() const noexcept { return config_digest_; }

private:
  void append(TraceChannel channel, const std::string& line, Sequence sequence);
  void write(TraceChannel channel, const Event& event, Sequence sequence, std::string_view kind,
             const std::string& payload, AgentId actor, OrderId correlation, Sequence causation);
  const RunConfig& config_;
  RecordSink& sink_;
  SequenceCounter sequence_;
  Sha256 commands_hash_;
  Sha256 trace_hash_;
  Sha256 trades_hash_;
  std::string config_digest_;
  std::uint64_t bytes_ = 0;
  std::string line_; // reused for every record
};

} // namespace markettwin
