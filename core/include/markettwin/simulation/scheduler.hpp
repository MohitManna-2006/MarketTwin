#ifndef MARKETTWIN_SIMULATION_SCHEDULER_HPP
#define MARKETTWIN_SIMULATION_SCHEDULER_HPP

#include "markettwin/common/run_error.hpp"
#include "markettwin/simulation/event.hpp"

#include <optional>
#include <queue>
#include <vector>

namespace markettwin {

struct SchedulerLimits {
  SimTime horizon{1'000'000'000};
  std::uint64_t max_events = 100'000;
  std::size_t max_queue = 100'000;
};

class Scheduler {
public:
  explicit Scheduler(SchedulerLimits limits = {});
  [[nodiscard]] Sequence schedule(SimTime timestamp, const EventPayload& payload);
  [[nodiscard]] Sequence schedule_after(SimTime delay, const EventPayload& payload);
  [[nodiscard]] std::optional<Event> next();
  // The time of the event next() would dispatch, or nothing when next() would stop.
  [[nodiscard]] std::optional<SimTime> peek() const;
  void request_stop() noexcept;
  [[nodiscard]] SimTime now() const noexcept { return now_; }
  [[nodiscard]] std::uint64_t dispatched() const noexcept { return dispatched_; }
  [[nodiscard]] std::size_t size() const noexcept { return queue_.size(); }
  [[nodiscard]] std::optional<StopReason> stop_reason() const noexcept { return stop_reason_; }

private:
  struct Later {
    bool operator()(const Event& left, const Event& right) const noexcept;
  };
  SchedulerLimits limits_;
  std::priority_queue<Event, std::vector<Event>, Later> queue_;
  SequenceCounter sequence_;
  SimTime now_;
  std::uint64_t dispatched_ = 0;
  std::optional<StopReason> stop_reason_;
};

} // namespace markettwin
#endif // MARKETTWIN_SIMULATION_SCHEDULER_HPP
