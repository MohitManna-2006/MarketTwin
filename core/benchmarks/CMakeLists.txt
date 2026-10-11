add_executable(markettwin_bench exchange_bench.cpp simulation_bench.cpp)
target_link_libraries(markettwin_bench PRIVATE markettwin::core benchmark::benchmark)
target_compile_features(markettwin_bench PRIVATE cxx_std_20)
target_compile_definitions(
  markettwin_bench PRIVATE "MARKETTWIN_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\""
)
markettwin_set_warnings(markettwin_bench)
markettwin_enable_sanitizers(markettwin_bench)
