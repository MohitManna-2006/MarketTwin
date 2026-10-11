#include "markettwin/simulation/kernel.hpp"

#include <benchmark/benchmark.h>

#ifndef MARKETTWIN_SOURCE_DIR
#error "MARKETTWIN_SOURCE_DIR must point at the repository root"
#endif

namespace {
using namespace markettwin;

// Complete in-memory run of the shipped baseline scenario, shortened to the given number
// of simulated seconds. Every canonical record is still built and hashed; only the file
// writes are absent. Items are dispatched kernel events.
void baseline_scenario(benchmark::State& state) {
  auto config = load_config(std::filesystem::path{MARKETTWIN_SOURCE_DIR} / "configs/baseline.json");
  config.limits.horizon = SimTime{static_cast<std::uint64_t>(state.range(0)) * 1'000'000'000};
  std::uint64_t events = 0;
  std::uint64_t trades = 0;
  for ([[maybe_unused]] auto _ : state) {
    NullSink sink;
    auto summary = run_kernel(config, sink);
    events += summary.event_count;
    trades += summary.trade_count;
    benchmark::DoNotOptimize(summary);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(events));
  state.counters["trades_per_run"] =
      static_cast<double>(trades) / static_cast<double>(state.iterations());
  state.SetLabel("simulated seconds per run: " + std::to_string(state.range(0)));
}
} // namespace

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables, misc-use-internal-linkage,
// readability-identifier-naming)
BENCHMARK(baseline_scenario)->Arg(300)->Arg(3600)->Unit(benchmark::kMillisecond);
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables, misc-use-internal-linkage,
// readability-identifier-naming)
