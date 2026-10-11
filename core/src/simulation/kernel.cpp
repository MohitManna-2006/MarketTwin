#include "markettwin/simulation/kernel.hpp"

#include "../telemetry/recorder.hpp"

namespace markettwin {
namespace {

void dispatch_order(const Event& event, Sequence sequence, MatchingEngine& exchange,
                    Recorder& recorder, RunSummary& summary) {
  auto order = std::get<Order>(event.command);
  order.arrival_time = event.timestamp;
  order.arrival_sequence = sequence;
  const auto report = exchange.submit(order);
  for (const auto& trade : report.trades) {
    recorder.outcome(event, sequence, "Trade", codec::trade(trade), trade.taker_agent_id,
                     trade.aggressor_order_id);
    recorder.record_trade(trade);
    ++summary.trade_count;
  }
  for (const auto& status : report.order_updates) {
    recorder.outcome(event, sequence, "OrderStatus", codec::status(status), status.owner,
                     status.order_id);
  }
}

void dispatch(const Event& event, Sequence sequence, MatchingEngine& exchange, Recorder& recorder,
              RunSummary& summary, Scheduler& scheduler) {
  if (std::holds_alternative<Order>(event.command)) {
    dispatch_order(event, sequence, exchange, recorder, summary);
  } else if (const auto* cancel = std::get_if<CancelCommand>(&event.command)) {
    const auto result = exchange.cancel(cancel->order_id, cancel->owner);
    recorder.outcome(event, sequence, "CancelResult", codec::status(result.order), cancel->owner,
                     cancel->order_id);
  } else if (const auto* snapshot = std::get_if<SnapshotCommand>(&event.command)) {
    recorder.outcome(event, sequence, "BookSnapshot",
                     codec::snapshot(exchange.snapshot(snapshot->depth)));
  } else {
    scheduler.request_stop();
  }
}
} // namespace

RunSummary run_kernel(const RunConfig& config, RecordSink& sink) {
  validate_config(config);
  Scheduler scheduler{config.limits};
  MatchingEngine exchange{config.exchange};
  Recorder recorder{config, sink};
  for (const auto& item : config.commands) {
    static_cast<void>(scheduler.schedule(item.timestamp, item.command));
  }
  RunSummary summary;
  summary.seed = config.seed;
  while (const auto event = scheduler.next()) {
    const auto sequence = recorder.command(*event);
    dispatch(*event, sequence, exchange, recorder, summary, scheduler);
  }
  exchange.verify_invariants();
  summary.event_count = scheduler.dispatched();
  const auto stop_reason = scheduler.stop_reason();
  if (!stop_reason) {
    throw RunError{"InternalError", "scheduler drained without a terminal reason"};
  }
  summary.stop_reason = *stop_reason;
  summary.final_time = scheduler.now();
  summary.resting_order_count = exchange.book().size();
  summary.book_digest = codec::book_digest(exchange);
  recorder.finish(summary);
  return summary;
}

} // namespace markettwin
