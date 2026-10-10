# Suite layout for later phases:
#   unit/exchange/     price-time matching and order validation
#   unit/simulation/   scheduler, clock, and kernel
#   integration/       cross-subsystem runs once those subsystems exist
#   property/          randomized invariant traces
#   replay/            digest equality and first-divergence checks

add_executable(
  markettwin_tests
  unit/exchange/matching_engine_test.cpp
  property/exchange_trace_test.cpp
  unit/version_test.cpp
  unit/simulation/simulation_test.cpp
)
target_link_libraries(markettwin_tests PRIVATE markettwin::core GTest::gtest_main)
target_compile_features(markettwin_tests PRIVATE cxx_std_20)
target_compile_definitions(
  markettwin_tests PRIVATE "MARKETTWIN_TEST_VERSION=\"${PROJECT_VERSION}\""
)
markettwin_set_warnings(markettwin_tests)
markettwin_enable_sanitizers(markettwin_tests)

include(GoogleTest)
gtest_discover_tests(
  markettwin_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES
    TIMEOUT 30
)

add_test(NAME markettwin_cli.version COMMAND markettwin --version)
set_tests_properties(
  markettwin_cli.version PROPERTIES PASS_REGULAR_EXPRESSION "MarketTwin ${PROJECT_VERSION}" TIMEOUT 30
)

add_test(NAME markettwin_cli.help COMMAND markettwin --help)
set_tests_properties(
  markettwin_cli.help PROPERTIES PASS_REGULAR_EXPRESSION "Usage: markettwin" TIMEOUT 30
)

add_test(NAME markettwin_cli.rejects_unknown COMMAND markettwin --run)
set_tests_properties(markettwin_cli.rejects_unknown PROPERTIES WILL_FAIL TRUE TIMEOUT 30)
