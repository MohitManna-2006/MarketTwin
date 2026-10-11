#pragma once

#include "markettwin/common/hash.hpp"
#include "markettwin/telemetry/event_log.hpp"

#include "codec.hpp"

namespace markettwin {

class Recorder {
public:
  Recorder(const RunConfig& config, RecordSink& sink);
  [[nodiscard]] Sequence command(const Event& event);
  void outcome(const Event& cause, Sequence causation, std::string_view kind,
               const codec::Json& payload, AgentId actor = {}, OrderId correlation = {});
  void finish(RunSummary& summary);
  void record_trade(const Trade& trade);

private:
  void write(TraceChannel channel, const codec::Json& record, Sequence sequence);
  [[nodiscard]] codec::Json envelope(const Event& event, Sequence sequence, std::string_view kind,
                                     const codec::Json& payload, AgentId actor, OrderId correlation,
                                     Sequence causation) const;
  const RunConfig& config_;
  RecordSink& sink_;
  SequenceCounter sequence_;
  Sha256 commands_hash_;
  Sha256 trace_hash_;
  Sha256 trades_hash_;
  std::string config_digest_;
  std::uint64_t bytes_ = 0;
};

} // namespace markettwin
