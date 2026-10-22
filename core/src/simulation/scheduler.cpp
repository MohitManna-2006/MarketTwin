#include "markettwin/simulation/scheduler.hpp"

#include <limits>

namespace markettwin {

Scheduler::Scheduler(SchedulerLimits limits) : limits_(limits) {
  if (limits_.max_events == 0 || limits_.max_queue == 0) {
    throw RunError{"InvalidConfig", "scheduler event and queue limits must be positive"};
  }
}

bool Scheduler::Later::operator()(const Event& left, const Event& right) const noexcept {
  if (left.timestamp != right.timestamp) {
    return left.timestamp > right.timestamp;
  }
  return left.scheduler_sequence > right.scheduler_sequence;
}

Sequence Scheduler::schedule(SimTime timestamp, const EventPayload& payload) {
  if (stop_reason_) {
    throw RunError{"SchedulerStopped", "cannot schedule into a terminated scheduler"};
  }
  if (timestamp < now_) {
    throw RunError{"PastEvent", "cannot schedule before the current logical clock"};
  }
  if (payload.valueless_by_exception()) {
    throw RunError{"InvalidPayload", "event command has no payload"};
  }
  if (queue_.size() >= limits_.max_queue) {
    throw RunError{"QueueCapacity", "scheduler queue capacity exceeded"};
  }
  const auto id = sequence_.next();
  queue_.push(Event{.timestamp = timestamp, .scheduler_sequence = id, .payload = payload});
  return id;
}

Sequence Scheduler::schedule_after(SimTime delay, const EventPayload& payload) {
  if (delay.value() > std::numeric_limits<std::uint64_t>::max() - now_.value()) {
    throw RunError{"TimeOverflow", "scheduled delay overflows logical time"};
  }
  return schedule(SimTime{now_.value() + delay.value()}, payload);
}

std::optional<Event> Scheduler::next() {
  if (stop_reason_) {
    return std::nullopt;
  }
  if (queue_.empty()) {
    stop_reason_ = StopReason::QueueEmpty;
  } else if (dispatched_ >= limits_.max_events) {
    stop_reason_ = StopReason::EventLimit;
  } else if (queue_.top().timestamp > limits_.horizon) {
    now_ = limits_.horizon;
    stop_reason_ = StopReason::Horizon;
  } else {
    auto event = queue_.top();
    queue_.pop();
    now_ = event.timestamp;
    ++dispatched_;
    return event;
  }
  return std::nullopt;
}

std::optional<SimTime> Scheduler::peek() const {
  if (stop_reason_ || queue_.empty() || dispatched_ >= limits_.max_events ||
      queue_.top().timestamp > limits_.horizon) {
    return std::nullopt;
  }
  return queue_.top().timestamp;
}

void Scheduler::request_stop() noexcept {
  stop_reason_ = StopReason::Requested;
}

} // namespace markettwin
