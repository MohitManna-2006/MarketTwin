add_library(markettwin_core src/common/version.cpp src/simulation/simulation.cpp)
add_library(markettwin::core ALIAS markettwin_core)

target_include_directories(
  markettwin_core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_features(markettwin_core PUBLIC cxx_std_20)
target_compile_definitions(markettwin_core PRIVATE "MARKETTWIN_VERSION=\"${PROJECT_VERSION}\"")

markettwin_set_warnings(markettwin_core)
markettwin_enable_sanitizers(markettwin_core)

add_executable(markettwin apps/markettwin_cli.cpp)
target_link_libraries(markettwin PRIVATE markettwin::core)
target_compile_features(markettwin PRIVATE cxx_std_20)
markettwin_set_warnings(markettwin)
markettwin_enable_sanitizers(markettwin)

if(MARKETTWIN_BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(MARKETTWIN_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
