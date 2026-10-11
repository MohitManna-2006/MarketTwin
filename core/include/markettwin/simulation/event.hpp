#ifndef MARKETTWIN_SIMULATION_EVENT_HPP
#define MARKETTWIN_SIMULATION_EVENT_HPP

#include "markettwin/exchange/order.hpp"

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
struct Event {
  SimTime timestamp;
  Sequence scheduler_sequence;
  Command command;
  bool operator==(const Event&) const = default;
};

enum class StopReason { QueueEmpty, Horizon, EventLimit, Requested };

} // namespace markettwin
#endif // MARKETTWIN_SIMULATION_EVENT_HPP
