#include "markettwin/common/version.hpp"

#include <gtest/gtest.h>
#include <string_view>

TEST(VersionTest, MatchesConfiguredProjectVersion) {
  EXPECT_FALSE(markettwin::version().empty());
  EXPECT_EQ(markettwin::version(), std::string_view{MARKETTWIN_TEST_VERSION});
}
