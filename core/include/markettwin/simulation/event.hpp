#ifndef MARKETTWIN_SIMULATION_EVENT_HPP
#define MARKETTWIN_SIMULATION_EVENT_HPP

#include "markettwin/agents/messages.hpp"

#include <cstddef>
#include <variant>

namespace markettwin {

struct CancelCommand {
  OrderId order_id;
  AgentId owner;
  bool operator==(const CancelCommand&) const = default;
};
struct SnapshotCommand {
  std::size_t depth = 10;
  bool operator==(const SnapshotCommand&) const = default;
};
struct StopCommand {
  bool operator==(const StopCommand&) const = default;
};
using Command = std::variant<Order, CancelCommand, SnapshotCommand, StopCommand>;

struct ScheduledCommand {
  SimTime timestamp;
  Command command;
  bool operator==(const ScheduledCommand&) const = default;
};
// Position of an agent in ScenarioConfig::agents. Deliveries carry it so routing an
// event to its agent is an index, not a lookup.
using AgentSlot = std::size_t;

struct AgentWakeup {
  AgentSlot slot = 0;
  bool operator==(const AgentWakeup&) const = default;
};
struct MarketDataDelivery {
  AgentSlot slot = 0;
  MarketDataUpdate update;
  bool operator==(const MarketDataDelivery&) const = default;
};
struct OrderReportDelivery {
  AgentSlot slot = 0;
  OrderReport report;
  bool operator==(const OrderReportDelivery&) const = default;
};
struct FillDelivery {
  AgentSlot slot = 0;
  Fill fill;
  bool operator==(const FillDelivery&) const = default;
};
struct FundamentalTick {
  bool operator==(const FundamentalTick&) const = default;
};
enum class ShockPhase { Instant, Begin, End };
struct ShockTransition {
  std::size_t shock = 0; // index into ScenarioConfig::shocks
  ShockPhase phase = ShockPhase::Instant;
  bool operator==(const ShockTransition&) const = default;
};
struct MetricSample {
  std::size_t depth = 1;
  bool operator==(const MetricSample&) const = default;
};

// Everything the scheduler can hold. The first four alternatives are exchange commands,
// in the same order as Command; the rest exist only in scenario runs.
using EventPayload = std::variant<Order, CancelCommand, SnapshotCommand, StopCommand, AgentWakeup,
                                  MarketDataDelivery, OrderReportDelivery, FillDelivery,
                                  FundamentalTick, ShockTransition, MetricSample>;

struct Event {
  SimTime timestamp;
  Sequence scheduler_sequence;
  EventPayload payload;
  bool operator==(const Event&) const = default;
};

enum class StopReason { QueueEmpty, Horizon, EventLimit, Requested };

} // namespace markettwin
#endif // MARKETTWIN_SIMULATION_EVENT_HPP
