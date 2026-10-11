#include "markettwin/telemetry/replay.hpp"

#include "../test_support.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

TEST(GoldenRunTest, Phase1bExampleMatchesVersionedDigests) {
  using namespace markettwin;
  const std::filesystem::path source{MARKETTWIN_SOURCE_DIR};
  const test::TempDirectory temporary;
  const auto config = load_config(source / "configs/phase1b_demo.json");
  const auto summary = Simulation{config, temporary.run()}.run();
  const auto expected =
      nlohmann::json::parse(test::read(source / "core/tests/replay/golden/phase1b_summary.json"));
  EXPECT_EQ(nlohmann::json::parse(summary_json(summary)), expected);
  const auto replay = verify_replay(temporary.run());
  EXPECT_TRUE(replay.verified) << replay_result_json(replay);
  EXPECT_EQ(replay.summary, summary);
}
