#include "markettwin/exchange/matching_engine.hpp"

#include <benchmark/benchmark.h>
#include <memory>
#include <vector>

namespace {
using namespace markettwin;
constexpr std::uint64_t kBatch = 512;

Order make_order(std::uint64_t id, Side side, std::uint64_t owner) {
  return {.id = OrderId{id},
          .owner = AgentId{owner},
          .symbol = SymbolId{1},
          .side = side,
          .type = OrderType::Limit,
          .limit_price = PriceTicks{100},
          .original_quantity = Quantity{1},
          .remaining_quantity = Quantity{1},
          .tif = TimeInForce::Gtc,
          .arrival_time = {},
          .arrival_sequence = Sequence{id}};
}

enum class Workload { NewOrder, Crossing, Cancel };

void exchange_batch(benchmark::State& state, Workload workload) {
  const ExchangeConfig config{.symbol = SymbolId{1}, .agents = {AgentId{1}, AgentId{2}}};
  std::vector<Order> commands;
  for (std::uint64_t id = 1; id <= kBatch; ++id) {
    auto order = make_order(kBatch + id, Side::Buy, 2);
    if (workload == Workload::NewOrder) {
      order.limit_price = PriceTicks{90};
    }
    commands.push_back(order);
  }
  for ([[maybe_unused]] auto _ : state) {
    state.PauseTiming();
    auto exchange = std::make_unique<MatchingEngine>(config);
    if (workload != Workload::NewOrder) {
      for (std::uint64_t id = 1; id <= kBatch; ++id) {
        static_cast<void>(exchange->submit(make_order(id, Side::Sell, 1)));
      }
    }
    state.ResumeTiming();
    for (const auto& command : commands) {
      if (workload == Workload::Cancel) {
        auto result = exchange->cancel(OrderId{command.id.value() - kBatch}, AgentId{1});
        benchmark::DoNotOptimize(result);
      } else {
        auto result = exchange->submit(command);
        benchmark::DoNotOptimize(result);
      }
    }
    state.PauseTiming();
    exchange.reset();
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kBatch));
  state.SetLabel("512 operations/batch; setup and teardown excluded");
}
} // namespace

// Google Benchmark registration macros generate globals and identifiers.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables, misc-use-internal-linkage,
// readability-identifier-naming)
BENCHMARK_CAPTURE(exchange_batch, new_order, Workload::NewOrder);
BENCHMARK_CAPTURE(exchange_batch, crossing, Workload::Crossing);
BENCHMARK_CAPTURE(exchange_batch, cancel, Workload::Cancel);
BENCHMARK_MAIN();
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables, misc-use-internal-linkage,
// readability-identifier-naming)
