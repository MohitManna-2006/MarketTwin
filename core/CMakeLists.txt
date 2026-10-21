add_library(markettwin_core
  src/common/version.cpp
  src/common/hash.cpp
  src/common/rng.cpp
  src/historical/historical.cpp
  src/historical/observed_values.cpp
  src/live/live.cpp
  src/accounting/money.cpp
  src/accounting/ledger.cpp
  src/accounting/valuation.cpp
  src/accounting/accounting_config.cpp
  src/accounting/accountant.cpp
  src/agents/agent.cpp
  src/agents/market_maker.cpp
  src/agents/momentum.cpp
  src/agents/value.cpp
  src/agents/noise.cpp
  src/simulation/config.cpp
  src/simulation/scenario.cpp
  src/simulation/fundamental.cpp
  src/simulation/population.cpp
  src/simulation/scheduler.cpp
  src/simulation/kernel.cpp
  src/telemetry/codec.cpp
  src/telemetry/scenario_codec.cpp
  src/telemetry/accounting_codec.cpp
  src/telemetry/metrics.cpp
  src/telemetry/recorder.cpp
  src/telemetry/artifacts.cpp
  src/telemetry/replay.cpp
  src/simulation/simulation.cpp
  src/experiment/statistics.cpp
  src/experiment/experiment.cpp
  src/calibration/calibration.cpp
  src/exchange/order_book.cpp
  src/exchange/matching_engine.cpp
)
add_library(markettwin::core ALIAS markettwin_core)
target_link_libraries(markettwin_core PRIVATE nlohmann_json::nlohmann_json)
execute_process(COMMAND git describe --always --dirty
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  OUTPUT_VARIABLE MARKETTWIN_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT MARKETTWIN_REVISION)
  set(MARKETTWIN_REVISION "unknown")
endif()
target_compile_definitions(markettwin_core PRIVATE
  "MARKETTWIN_GIT_REVISION=\"${MARKETTWIN_REVISION}\""
  "MARKETTWIN_COMPILER=\"${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\""
  "MARKETTWIN_BUILD_TYPE=\"${CMAKE_BUILD_TYPE}\"")

target_include_directories(
  markettwin_core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_features(markettwin_core PUBLIC cxx_std_20)
target_compile_definitions(markettwin_core PRIVATE "MARKETTWIN_VERSION=\"${PROJECT_VERSION}\"")

markettwin_set_warnings(markettwin_core)
markettwin_enable_sanitizers(markettwin_core)

add_executable(markettwin apps/markettwin_cli.cpp)
target_link_libraries(markettwin PRIVATE markettwin::core nlohmann_json::nlohmann_json)
target_compile_features(markettwin PRIVATE cxx_std_20)
markettwin_set_warnings(markettwin)
markettwin_enable_sanitizers(markettwin)

if(MARKETTWIN_BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(MARKETTWIN_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
