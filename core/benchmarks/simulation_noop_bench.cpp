#include "markettwin/simulation/simulation.hpp"

#include <benchmark/benchmark.h>

namespace {

void noop_run(benchmark::State& state) {
  const markettwin::RunConfig config{.seed = 1};
  for ([[maybe_unused]] auto _ : state) {
    markettwin::Simulation simulation{config};
    markettwin::RunSummary summary = simulation.run();
    benchmark::DoNotOptimize(summary);
  }
}

} // namespace

// Google Benchmark's registration macros expand to globals with generated names.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables, misc-use-internal-linkage,
// readability-identifier-naming)
BENCHMARK(noop_run);
BENCHMARK_MAIN();
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables, misc-use-internal-linkage,
// readability-identifier-naming)
