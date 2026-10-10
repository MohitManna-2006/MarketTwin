#include "markettwin/simulation/simulation.hpp"

#include <gtest/gtest.h>
#include <type_traits>

TEST(SimulationTest, NoOpRunCarriesTheConfiguredSeed) {
  markettwin::Simulation simulation{markettwin::RunConfig{.seed = 7}};

  const markettwin::RunSummary summary = simulation.run();

  EXPECT_EQ(summary.status, markettwin::RunStatus::Completed);
  EXPECT_EQ(summary.event_count, 0U);
  EXPECT_EQ(summary.seed, 7U);
}

TEST(SimulationTest, RepeatedRunsStayIdentical) {
  markettwin::Simulation first{markettwin::RunConfig{.seed = 11}};
  markettwin::Simulation second{markettwin::RunConfig{.seed = 11}};

  const markettwin::RunSummary first_summary = first.run();
  const markettwin::RunSummary second_summary = second.run();
  const markettwin::RunSummary repeated_summary = first.run();

  EXPECT_EQ(first_summary.status, second_summary.status);
  EXPECT_EQ(first_summary.event_count, second_summary.event_count);
  EXPECT_EQ(first_summary.seed, second_summary.seed);
  EXPECT_EQ(repeated_summary.event_count, first_summary.event_count);
  EXPECT_EQ(repeated_summary.seed, first_summary.seed);
}

TEST(SimulationTest, DistinctSeedsStayDistinct) {
  markettwin::Simulation low{markettwin::RunConfig{.seed = 1}};
  markettwin::Simulation high{markettwin::RunConfig{.seed = 2}};

  EXPECT_NE(low.run().seed, high.run().seed);
}

TEST(SimulationTest, ArtifactsStayEmptyBeforeTelemetryExists) {
  static_assert(std::is_empty_v<markettwin::RunArtifacts>);

  markettwin::Simulation simulation{markettwin::RunConfig{}};
  static_cast<void>(simulation.artifacts());
  EXPECT_EQ(simulation.run().event_count, 0U);
}
