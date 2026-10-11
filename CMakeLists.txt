cmake_minimum_required(VERSION 3.25)

project(
  MarketTwin
  VERSION 0.3.0
  DESCRIPTION "Deterministic C++20 agent-based synthetic equity exchange"
  LANGUAGES CXX
)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE
      Debug
      CACHE STRING "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE
  )
  set_property(
    CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel
  )
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MARKETTWIN_BUILD_TESTS "Build the GoogleTest suite" ON)
option(MARKETTWIN_BUILD_BENCHMARKS "Build Google Benchmark smoke targets" OFF)
option(MARKETTWIN_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(MARKETTWIN_WARNINGS_AS_ERRORS "Treat compiler warnings as errors on MarketTwin targets" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)
include(Dependencies)

if(MARKETTWIN_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(core)
