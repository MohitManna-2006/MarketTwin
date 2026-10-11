#include "markettwin/simulation/scheduler.hpp"

#include <gtest/gtest.h>
#include <limits>

namespace markettwin {
namespace {

TEST(SchedulerTest, OrdersByTimeThenInsertionAndSupportsDynamicSameTimeInsertion) {
  Scheduler scheduler;
  EXPECT_EQ(scheduler.schedule(SimTime{20}, SnapshotCommand{1}), Sequence{1});
  EXPECT_EQ(scheduler.schedule(SimTime{10}, SnapshotCommand{2}), Sequence{2});
  EXPECT_EQ(scheduler.schedule(SimTime{10}, SnapshotCommand{3}), Sequence{3});
  const auto first = scheduler.next();
  ASSERT_TRUE(first);
  EXPECT_EQ(first.value_or(Event{}).scheduler_sequence, Sequence{2});
  EXPECT_EQ(scheduler.now(), SimTime{10});
  EXPECT_EQ(scheduler.schedule_after(SimTime{}, SnapshotCommand{4}), Sequence{4});
  EXPECT_EQ(scheduler.next().value_or(Event{}).scheduler_sequence, Sequence{3});
  EXPECT_EQ(scheduler.next().value_or(Event{}).scheduler_sequence, Sequence{4});
  EXPECT_EQ(scheduler.next().value_or(Event{}).scheduler_sequence, Sequence{1});
  EXPECT_FALSE(scheduler.next());
  EXPECT_EQ(scheduler.stop_reason(), StopReason::QueueEmpty);
  EXPECT_EQ(scheduler.dispatched(), 4U);
}

TEST(SchedulerTest, RejectsPastQueueOverflowAndTimeOverflowWithoutConsumingSequence) {
  Scheduler scheduler{{.horizon = SimTime{std::numeric_limits<std::uint64_t>::max()},
                       .max_events = 10,
                       .max_queue = 1}};
  EXPECT_EQ(scheduler.schedule(SimTime{10}, StopCommand{}), Sequence{1});
  EXPECT_THROW(static_cast<void>(scheduler.schedule(SimTime{20}, StopCommand{})), RunError);
  EXPECT_TRUE(scheduler.next());
  EXPECT_THROW(static_cast<void>(scheduler.schedule(SimTime{9}, StopCommand{})), RunError);
  EXPECT_THROW(static_cast<void>(scheduler.schedule_after(
                   SimTime{std::numeric_limits<std::uint64_t>::max()}, StopCommand{})),
               RunError);
  EXPECT_EQ(scheduler.schedule(SimTime{10}, StopCommand{}), Sequence{2});
  EXPECT_EQ(scheduler.size(), 1U);
}

TEST(SchedulerTest, HorizonIsInclusiveAndDoesNotDispatchBeyondIt) {
  Scheduler scheduler{{.horizon = SimTime{10}, .max_events = 3, .max_queue = 3}};
  static_cast<void>(scheduler.schedule(SimTime{11}, StopCommand{}));
  static_cast<void>(scheduler.schedule(SimTime{10}, SnapshotCommand{}));
  EXPECT_TRUE(scheduler.next());
  EXPECT_FALSE(scheduler.next());
  EXPECT_EQ(scheduler.now(), SimTime{10});
  EXPECT_EQ(scheduler.stop_reason(), StopReason::Horizon);
  EXPECT_EQ(scheduler.size(), 1U);
}

TEST(SchedulerTest, EventCapAndRequestedStopAreTerminal) {
  Scheduler scheduler{{.horizon = SimTime{10}, .max_events = 1, .max_queue = 2}};
  static_cast<void>(scheduler.schedule(SimTime{}, SnapshotCommand{}));
  static_cast<void>(scheduler.schedule(SimTime{1}, SnapshotCommand{}));
  EXPECT_TRUE(scheduler.next());
  EXPECT_FALSE(scheduler.next());
  EXPECT_EQ(scheduler.stop_reason(), StopReason::EventLimit);
  EXPECT_THROW(static_cast<void>(scheduler.schedule(SimTime{1}, SnapshotCommand{})), RunError);
  Scheduler stopped;
  static_cast<void>(stopped.schedule(SimTime{}, SnapshotCommand{}));
  stopped.request_stop();
  EXPECT_FALSE(stopped.next());
  EXPECT_EQ(stopped.stop_reason(), StopReason::Requested);
  EXPECT_EQ(stopped.dispatched(), 0U);
}

TEST(SchedulerTest, EmptyQueuePrecedesEventCapAndInvalidLimitsFail) {
  Scheduler scheduler{{.horizon = SimTime{}, .max_events = 1, .max_queue = 1}};
  static_cast<void>(scheduler.schedule(SimTime{}, SnapshotCommand{}));
  EXPECT_TRUE(scheduler.next());
  EXPECT_FALSE(scheduler.next());
  EXPECT_EQ(scheduler.stop_reason(), StopReason::QueueEmpty);
  EXPECT_THROW(Scheduler({.horizon = SimTime{}, .max_events = 0, .max_queue = 1}), RunError);
  EXPECT_THROW(Scheduler({.horizon = SimTime{}, .max_events = 1, .max_queue = 0}), RunError);
}

TEST(SchedulerTest, SequenceExhaustionNeverWraps) {
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  SequenceCounter counter{Sequence{maximum - 1}};
  EXPECT_EQ(counter.next(), Sequence{maximum});
  EXPECT_THROW(static_cast<void>(counter.next()), RunError);
  EXPECT_THROW(static_cast<void>(counter.next()), RunError);
}

} // namespace
} // namespace markettwin
