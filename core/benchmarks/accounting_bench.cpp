#include "markettwin/accounting/valuation.hpp"
#include "markettwin/experiment/statistics.hpp"

#include <benchmark/benchmark.h>
#include <vector>

namespace {
using namespace markettwin;
constexpr std::uint64_t kAgents = 8;
constexpr std::uint64_t kBatch = 4'096;

std::int64_t items(const benchmark::State& state, std::uint64_t per_iteration) {
  return state.iterations() * static_cast<std::int64_t>(per_iteration);
}

Ledger make_ledger() {
  std::vector<AccountOpening> openings;
  for (std::uint64_t id = 1; id <= kAgents; ++id) {
    openings.push_back({.agent = AgentId{id}, .cash = {}, .position = 0, .cost_basis = {}});
  }
  return Ledger{std::move(openings), {.maker_ppm = -20, .taker_ppm = 30}, 1};
}

// Trades that rotate through the agents and across a few prices, so positions open,
// shrink, close, and reverse instead of only growing.
std::vector<Trade> make_trades() {
  std::vector<Trade> trades;
  trades.reserve(kBatch);
  for (std::uint64_t id = 1; id <= kBatch; ++id) {
    const auto buyer = (id % kAgents) + 1;
    // Another agent: an offset of 1 to kAgents - 1 from the buyer, wrapping around.
    const auto offset = (((id * 5) + 3) % (kAgents - 1)) + 1;
    const auto seller = ((buyer - 1 + offset) % kAgents) + 1;
    trades.push_back({
        .id = TradeId{id},
        .symbol = SymbolId{1},
        .buy_order_id = OrderId{2 * id},
        .sell_order_id = OrderId{(2 * id) + 1},
        .aggressor_order_id = OrderId{2 * id},
        .resting_order_id = OrderId{(2 * id) + 1},
        .maker_agent_id = AgentId{seller},
        .taker_agent_id = AgentId{buyer},
        .price_ticks = PriceTicks{15'000 + static_cast<std::int64_t>(id % 7)},
        .quantity = Quantity{1 + (id % 40)},
        .timestamp = {},
        .sequence = Sequence{id},
        .arrival_sequence = Sequence{id},
    });
  }
  return trades;
}

// Booking one execution into both parties' accounts, fees included. Ledger construction
// is outside the timed region. Items are trades.
void ledger_apply(benchmark::State& state) {
  const auto trades = make_trades();
  for ([[maybe_unused]] auto _ : state) {
    state.PauseTiming();
    auto ledger = make_ledger();
    state.ResumeTiming();
    for (const auto& trade : trades) {
      benchmark::DoNotOptimize(ledger.apply(trade));
    }
  }
  state.SetItemsProcessed(items(state, kBatch));
}

// Marking every account to a midpoint, as each time-series sample does. Items are accounts.
void mark_accounts(benchmark::State& state) {
  auto ledger = make_ledger();
  for (const auto& trade : make_trades()) {
    static_cast<void>(ledger.apply(trade));
  }
  const Mark mark{.source = MarkSource::Midpoint, .twice_price_ticks = 30'007};
  for ([[maybe_unused]] auto _ : state) {
    for (const auto& account : ledger.accounts()) {
      benchmark::DoNotOptimize(value_account(account, mark, 1));
    }
  }
  state.SetItemsProcessed(items(state, kAgents));
}

// Summary, Student t interval, and a 10,000-resample bootstrap over the given number of
// paired differences. Items are summaries.
void paired_statistics(benchmark::State& state) {
  std::vector<std::int64_t> differences;
  differences.reserve(static_cast<std::size_t>(state.range(0)));
  for (std::int64_t index = 0; index < state.range(0); ++index) {
    differences.push_back(((index * 7'919) % 20'001) - 9'000);
  }
  const BootstrapConfig bootstrap{.resamples = 10'000, .seed = 1};
  for ([[maybe_unused]] auto _ : state) {
    benchmark::DoNotOptimize(summarize(differences, bootstrap));
  }
  state.SetItemsProcessed(state.iterations());
}
} // namespace

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables, misc-use-internal-linkage,
// readability-identifier-naming)
BENCHMARK(ledger_apply);
BENCHMARK(mark_accounts);
BENCHMARK(paired_statistics)->Arg(200)->Unit(benchmark::kMillisecond);
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables, misc-use-internal-linkage,
// readability-identifier-naming)
