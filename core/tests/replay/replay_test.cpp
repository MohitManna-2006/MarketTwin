#include "markettwin/common/hash.hpp"
#include "markettwin/telemetry/replay.hpp"

#include "../test_support.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace markettwin {
namespace {
using Json = nlohmann::json;

TEST(ReplayTest, IndependentRunsProduceByteIdenticalArtifactsAndMatchEveryDigest) {
  const test::TempDirectory first;
  const test::TempDirectory second;
  Simulation simulation{test::fixture(), first.run()};
  const auto summary = simulation.run();
  const auto repeated = Simulation{test::fixture(), second.run()}.run();
  EXPECT_EQ(summary, repeated);
  EXPECT_EQ(summary.event_count, 5U);
  EXPECT_EQ(summary.trade_count, 1U);
  EXPECT_EQ(summary.resting_order_count, 0U);
  EXPECT_EQ(summary.final_time, SimTime{20});
  for (const auto* name : {
           "config.json",
           "commands.jsonl",
           "trace.jsonl",
           "accounting.jsonl",
           "metrics.json",
           "manifest.json",
       }) {
    EXPECT_EQ(test::read(first.run() / name), test::read(second.run() / name));
  }
  EXPECT_EQ(summary.commands_digest, sha256(test::read(simulation.artifacts().commands)));
  EXPECT_EQ(summary.trace_digest, sha256(test::read(simulation.artifacts().trace)));
  const auto replay = verify_replay(first.run());
  ASSERT_TRUE(replay.verified) << replay_result_json(replay);
  EXPECT_EQ(replay.summary, summary);
  EXPECT_EQ(Simulation{test::fixture()}.run(), summary);
}

TEST(ReplayTest, FirstCorruptedTradeReportsSequenceExpectedActualAndCausalContext) {
  const test::TempDirectory directory;
  static_cast<void>(Simulation{test::fixture(), directory.run()}.run());
  const auto trace_path = directory.run() / "trace.jsonl";
  auto bytes = test::read(trace_path);
  const auto trade_pos = bytes.find(R"("kind":"Trade")");
  ASSERT_NE(trade_pos, std::string::npos);
  const auto quantity_pos = bytes.find("\"quantity\":3", trade_pos);
  ASSERT_NE(quantity_pos, std::string::npos);
  bytes.replace(quantity_pos, 12, "\"quantity\":4");
  test::write(trace_path, bytes);
  const auto result = verify_replay(directory.run());
  EXPECT_FALSE(result.verified);
  EXPECT_EQ(result.code, "TraceMismatch");
  EXPECT_EQ(result.sequence, Sequence{4});
  EXPECT_NE(result.expected.find("\"quantity\":4"), std::string::npos);
  EXPECT_NE(result.actual.find("\"quantity\":3"), std::string::npos);
  EXPECT_NE(result.previous_command.find("OrderArrival"), std::string::npos);
  EXPECT_NE(result.previous_output.find("OrderStatus"), std::string::npos);
}

TEST(ReplayTest, ChangedConfigFailsAtFirstCommandBeforeMutatingExchange) {
  const test::TempDirectory directory;
  static_cast<void>(Simulation{test::fixture(), directory.run()}.run());
  auto config = test::fixture();
  std::get<Order>(config.commands.at(1).command).original_quantity = Quantity{6};
  std::get<Order>(config.commands.at(1).command).remaining_quantity = Quantity{6};
  test::write(directory.run() / "config.json", canonical_config(config) + '\n');
  const auto result = verify_replay(directory.run());
  EXPECT_FALSE(result.verified);
  // The linked config digest detects the change before even the first command.
  EXPECT_EQ(result.code, "TraceMismatch");
  EXPECT_EQ(result.sequence, Sequence{});
}

TEST(ReplayTest, ChangedCommandLogFailsAtItsDispatchSequence) {
  const test::TempDirectory directory;
  static_cast<void>(Simulation{test::fixture(), directory.run()}.run());
  const auto path = directory.run() / "commands.jsonl";
  auto bytes = test::read(path);
  const auto pos = bytes.find("\"quantity\":5");
  ASSERT_NE(pos, std::string::npos);
  bytes.replace(pos, 12, "\"quantity\":6");
  test::write(path, bytes);
  const auto result = verify_replay(directory.run());
  EXPECT_EQ(result.code, "TraceMismatch");
  EXPECT_EQ(result.sequence, Sequence{1});
}

TEST(ReplayTest, MissingTruncatedAndExtraRecordsAreExplicitFailures) {
  const test::TempDirectory root;
  EXPECT_EQ(verify_replay(root.run()).code, "IncompleteArtifacts");
  static_cast<void>(Simulation{test::fixture(), root.run()}.run());
  const auto path = root.run() / "trace.jsonl";
  const auto original = test::read(path);
  test::write(path, original.substr(0, original.size() - 1));
  EXPECT_EQ(verify_replay(root.run()).code, "IncompleteTrace");
  test::write(path, original + "{}\n");
  EXPECT_EQ(verify_replay(root.run()).code, "ExtraRecords");
  test::write(path, original);
  std::filesystem::remove(root.run() / "commands.jsonl");
  EXPECT_EQ(verify_replay(root.run()).code, "IncompleteArtifacts");
}

TEST(ReplayTest, RejectsSchemaEngineChecksumAndSummaryTampering) {
  const test::TempDirectory root;
  static_cast<void>(Simulation{test::fixture(), root.run()}.run());
  const auto path = root.run() / "manifest.json";
  const auto original = Json::parse(test::read(path));
  auto changed = original;
  changed.at("schema_version") = 3;
  test::write(path, changed.dump());
  EXPECT_EQ(verify_replay(root.run()).code, "UnsupportedSchema");
  changed = original;
  changed.at("engine_version") = "0.0.0";
  test::write(path, changed.dump());
  EXPECT_EQ(verify_replay(root.run()).code, "UnsupportedVersion");
  changed = original;
  changed.at("artifacts").at("trace.jsonl") = std::string(64, '0');
  test::write(path, changed.dump());
  EXPECT_EQ(verify_replay(root.run()).code, "ChecksumMismatch");
  changed = original;
  changed.at("summary").at("book_digest") = std::string(64, '0');
  test::write(path, changed.dump());
  EXPECT_EQ(verify_replay(root.run()).code, "SummaryMismatch");
}

TEST(ReplayTest, ExistingDirectoriesAreNeverOverwrittenAndFailedRunsHaveNoManifest) {
  const test::TempDirectory root;
  const auto config = test::fixture();
  static_cast<void>(Simulation{config, root.run()}.run());
  const auto manifest = test::read(root.run() / "manifest.json");
  EXPECT_THROW(static_cast<void>(Simulation{config, root.run()}.run()), RunError);
  EXPECT_EQ(test::read(root.run() / "manifest.json"), manifest);
  auto limited = config;
  limited.max_trace_bytes = 100;
  const auto failed = root.path() / "failed";
  EXPECT_THROW(static_cast<void>(Simulation{limited, failed}.run()), RunError);
  EXPECT_FALSE(std::filesystem::exists(failed / "manifest.json"));
  EXPECT_EQ(verify_replay(failed).code, "IncompleteArtifacts");
}

TEST(ReplayTest, FinalBookDigestIncludesOwnersAndFifoNotOnlyAggregates) {
  auto config = test::fixture();
  config.commands = {
      test::at(0, test::order(1, Side::Sell)),
      test::at(0, test::order(2, Side::Sell)),
  };
  const auto first = Simulation{config}.run();
  std::swap(config.commands.at(0), config.commands.at(1));
  const auto reordered = Simulation{config}.run();
  EXPECT_NE(first.book_digest, reordered.book_digest);
  std::get<Order>(config.commands.at(0).command).owner = AgentId{2};
  EXPECT_NE(reordered.book_digest, Simulation{config}.run().book_digest);
}

TEST(ReplayTest, EmptyCancelOnlyAndEachStopReasonReplay) {
  const test::TempDirectory root;
  for (int variant = 0; variant < 5; ++variant) {
    RunConfig config;
    if (variant == 1) {
      config.commands = {test::at(0, test::cancel(1, 1))};
    }
    if (variant == 2) {
      config.commands = {test::at(0, StopCommand{}), test::at(0, test::order(1, Side::Buy))};
    }
    if (variant == 3) {
      config.limits.horizon = SimTime{1};
      config.commands = {test::at(2, StopCommand{})};
    }
    if (variant == 4) {
      config.limits.max_events = 1;
      config.commands = {test::at(0, SnapshotCommand{}), test::at(1, StopCommand{})};
    }
    const auto path = root.path() / std::to_string(variant);
    const auto summary = Simulation{config, path}.run();
    const auto replay = verify_replay(path);
    EXPECT_TRUE(replay.verified) << replay_result_json(replay);
    EXPECT_EQ(summary, replay.summary);
    if (variant == 2) {
      EXPECT_EQ(summary.stop_reason, StopReason::Requested);
      EXPECT_EQ(summary.event_count, 1U);
    }
    if (variant == 3) {
      EXPECT_EQ(summary.stop_reason, StopReason::Horizon);
      EXPECT_EQ(summary.final_time, SimTime{1});
    }
    if (variant == 4) {
      EXPECT_EQ(summary.stop_reason, StopReason::EventLimit);
    }
  }
}

} // namespace
} // namespace markettwin
