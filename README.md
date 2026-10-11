# MarketTwin

Digital twin for market simulation.

MarketTwin is a research platform for studying how independent trading strategies interact inside a synthetic continuous double-auction equity exchange. The market-critical path is **C++20**: order matching, logical time, trading agents, accounting, experiment statistics, and deterministic event replay live in the native core. Python will orchestrate research and AI workflows. A React and TypeScript dashboard will display results. None of those later surfaces are allowed to compute authoritative prices, fills, or P&L.

There is no brokerage integration and no live order routing.

The complete technical contract is [`architecture.md`](architecture.md).

## Architecture

```text
React / TypeScript dashboard          (planned)
        |  REST and WebSocket
FastAPI research API                  (planned)
        |  coarse-grained jobs
Python research and AI tools          (planned)
        |  pybind11 or the native CLI, once per run
C++20 simulation                      (implemented: exchange, kernel, agents, scenarios, ledger,
        |                              market statistics, experiments, trace, replay)
        one logical thread per run
        event scheduler, matching engine, agents, accounting, telemetry
```

Subsystem boundaries (exchange, simulation kernel, agents, accounting, and telemetry implemented, with paired experiments in the native CLI; later subsystems planned):

| Area | Responsibility |
| --- | --- |
| Exchange | Order validation, price-time matching, cancels, executions |
| Simulation | Logical clock, event heap, latency, scenario dispatch |
| Agents | Strategy policies that emit intents, not book mutations |
| Accounting | Cash, inventory, P&L, fees |
| Telemetry | Event log, snapshots, replay digests |
| Bindings | Coarse-grained run configuration and summaries for Python |
| Research | Paired experiments, statistics, reports |
| API | Job control and queries |
| Web | Read-oriented operator console |

A single run stays deterministic and logically single-threaded. Independent runs may later execute in separate processes. Kafka, Kubernetes, and other distributed infrastructure are out of scope for the MVP.

## Current status

**Phase 2B — accounting, market statistics, and paired experiments.** Every execution is posted to an integer double-entry ledger as it happens, each run publishes its books, a time series, and market microstructure statistics next to its trace, and replay verifies all of it. A batch runner compares strategy variants over matched seeds and reports paired differences with confidence intervals. The shipped comparison of a baseline and an inventory-aware market maker runs 400 one-hour markets in about two and a half minutes on one core of an M1 Pro.

Implemented now:

- C++20 library target `markettwin::core` and the `markettwin` CLI (`run`, `replay`, `experiment`, `--version`)
- Strong integer price, quantity, identity, timestamp, and sequence types
- Single-symbol `MatchingEngine` with ordered bid/ask maps, FIFO lists, and an ID index
- Limit GTC/IOC and market IOC orders, partial fills, and residual cancellation
- Structured execution/status reports, transactional rejection, and self-trade prevention
- Immutable depth snapshots, optional best quotes, and full book/index invariant checks
- Heap scheduler ordered by `(timestamp, scheduler sequence)` with an inclusive horizon, event and queue caps, and recorded stop reasons
- Strict versioned JSON run configs: unknown or missing fields, duplicate keys, floats, and out-of-range numbers are rejected
- Canonical `config.json`, `commands.jsonl`, `trace.jsonl`, `accounting.jsonl`, `metrics.json`, and a `manifest.json` with SHA-256 digests, published only after a completed run
- Replay that regenerates the run from its config and seed, byte-checks every command before dispatch, every output after it, and every ledger posting and sample, and checks final book and trade digests
- Trading policies: baseline market maker, inventory-aware market maker, momentum, value, and noise
- A delivered-observation boundary: per-agent order-entry, report, and market-data latency, with per-agent position, order-size, open-order, and message limits
- xoshiro256\*\* random streams per consumer with integer-only samplers, so a seed gives the same run on every toolchain
- A mean-reverting synthetic fundamental value and three shocks: fundamental jump, liquidity withdrawal, and volatility regime
- A per-agent ledger in integer minor units: cash, signed position, average-cost basis, realized and unrealized P&L, fees and rebates through a fee sink, with checked arithmetic and exact conservation of cash and shares
- Mark-to-market at the midpoint, else the last trade, else the scenario reference, with the source recorded; equity, peak, and drawdown on the sampling schedule
- Market statistics: spread, relative spread, top-of-book and N-level depth, volume, VWAP, fill rates, return volatility, exact time in each book state, rejections and cancels, and maker spread capture
- An optional per-agent credit limit enforced before a buy intent becomes an order
- A deterministic experiment runner: seeds times strategy variants, per-run failure reporting, output budgets, paired differences with Student t and bootstrap intervals in integer arithmetic
- Unit tests, 32 seeded randomized exchange traces checked against an independent reference matcher, policy and scenario tests, accounting and statistics tests, replay corruption tests, and golden fixtures held across Apple Clang and GCC
- Optional AddressSanitizer and UndefinedBehaviorSanitizer build
- Optional Google Benchmark target for exchange operations, ledger postings, statistics, and complete scenario runs

Not implemented:

- A cash check at the exchange: the credit limit is applied to an agent's intents, and a fill is never rejected for lack of cash
- Order-flow imbalance, latency degradation, and quote-withdrawal shocks
- Parquet artifacts, effective-spread and adverse-selection metrics, Sharpe ratios, and parallel execution of runs
- Python bindings, FastAPI, PostgreSQL, Redis, or the React app
- AI research tools

The scenarios are stylized. Prices come from the interaction of the configured agents, not from market data, and nothing here is calibrated to a real stock or says anything about real-world profitability.

## Running a simulation

```bash
./build/release/core/markettwin run --config configs/baseline.json --output data/baseline
./build/release/core/markettwin replay --input data/baseline
```

`run` prints the terminal summary as one JSON line and writes six files into a **new** directory. It refuses an existing directory, so a run is never overwritten or appended to.

| File | Contents |
| --- | --- |
| `config.json` | Canonical form of the input config; its digest is linked from both logs |
| `commands.jsonl` | Header, then each dispatched command in dispatch order (inputs to the exchange) |
| `trace.jsonl` | Header, then trades, order statuses, cancel results, snapshots, fundamental updates, shock transitions, and the terminal record |
| `accounting.jsonl` | Header with units and conventions, then one ledger entry per trade (both parties' balances after it) and one time-series sample per depth snapshot (top of book, depth, mark and its source, fundamental, and every agent's cash, position, basis, P&L, and equity) |
| `metrics.json` | Final per-agent books and order flow, market statistics, conservation totals, and the final mark, with units |
| `manifest.json` | Engine version, build provenance, terminal summary, the schema version of each stream, and artifact checksums; written last |

`replay` reruns the simulation from `config.json`, including every agent decision, requires each command to match `commands.jsonl` byte for byte before it reaches a fresh exchange, and compares each output with `trace.jsonl` and each ledger posting and sample with `accounting.jsonl`. It exits 0 and prints `"verified":true` when everything agrees, including `metrics.json`, the final book digest, trade digest, and file checksums. A difference in the books alone is reported as `AccountingMismatch` with the first differing record. On the first difference it exits 1 and reports the trace sequence, the expected and actual records, the causal command, and the last verified output. A run that failed part-way has no manifest and does not verify.

### Scenario configs

| Config | What it runs |
| --- | --- |
| `configs/baseline.json` | One simulated hour: two baseline makers, one inventory-aware maker, two value traders, two momentum traders, four noise traders |
| `configs/fundamental_shock.json` | The baseline with the fundamental dropping 150 ticks at 30 minutes |
| `configs/liquidity_shock.json` | The baseline with both baseline makers withdrawn from minute 20 to minute 30 |
| `configs/volatility_shock.json` | The baseline with fundamental volatility quadrupled from minute 20 to minute 40 |
| `configs/baseline_accounting.json` | The baseline with a 30 ppm taker fee, a 20 ppm maker rebate, opening cash, and a zero credit limit on the three makers |
| `configs/phase1b_demo.json` | A scripted run with no agents: every order, cancel, snapshot, and stop comes from the config |
| `configs/experiments/maker_comparison.json` | An experiment, not a run: baseline versus inventory-aware maker over 200 seeds of the baseline |

The baseline and the three shock configs share a seed, so until its shock begins each variant is the same market as the baseline.

A scenario config (`schema_version` 2) is a scripted config plus a `scenario` object: a public reference price, the fundamental process, optional periodic depth sampling, the agents, and the shocks. Each agent has an ID registered with the exchange, a wakeup schedule (fixed interval or exponential), three latencies, hard limits, and a policy with its parameters. All values are integers: prices in ticks, sub-tick values in milliticks, times in nanoseconds, and fractions in parts per million. Malformed or out-of-range configs fail before the run starts.

A config that sets fees, opening cash, or credit limits is `schema_version` 3: the same fields plus an `accounting` object, with `scenario` allowed to be `null`. A config without one means no fees, zero opening cash, and unconstrained cash, and keeps the digest it had before Phase 2B.

The summary's `scenario` object reports, per agent, wakeups, orders and cancels sent, intents refused by the agent's own limits, fills, filled quantity, and net position in shares, along with the final fundamental value and a digest of every random stream's final state.

`configs/phase1b_demo.json` remains the reference scripted example: out-of-order scheduling, same-timestamp commands, a multi-level fill, a rejected foreign cancel, IOC residuals, an invalid order, and an explicit stop that leaves a later same-time order undispatched. Orders that are well-formed but invalid for the exchange, such as zero quantity or an unknown owner, run and are logged as rejections. The kernel assigns arrival time and sequence; configs cannot.

Golden summaries for the scripted demo and the baseline scenario are pinned in `core/tests/replay/golden/` and hold across Apple Clang and GCC builds.

[ADR 0002](docs/adr/0002-kernel-trace-and-replay.md) specifies ordering, stop conditions, the record format, digests, and replay failure codes. [ADR 0003](docs/adr/0003-agents-rng-latency-and-scenarios.md) specifies the agents, what they can observe, latency, random streams, the fundamental process, and shocks. [ADR 0004](docs/adr/0004-ledger-marks-metrics-and-experiments.md) specifies the ledger, marks, market statistics, the accounting stream, and experiments.

## Accounting and metrics

Money is a signed 64-bit count of minor currency units (cents, say). A tick is worth `tick_size_minor` minor units, so a trade's notional is `price_ticks × quantity × tick_size_minor`, exactly. Nothing in the ledger, the valuation, or the statistics is floating point; every operation is checked, and an overflow fails the run instead of wrapping.

| Quantity | Definition |
| --- | --- |
| Cash | Opening cash, minus notional bought, plus notional sold, minus fees |
| Position | Signed shares; short positions and negative cash are allowed unless a credit limit is set |
| Cost basis | Total signed entry value of the open position (average cost, never rounded per share) |
| Realized P&L | On shares released: proceeds minus the basis released, gross of fees |
| Unrealized P&L | Position marked to market, minus cost basis |
| Equity | Cash plus marked position |
| Total P&L | Equity minus opening equity, which equals realized − fees + unrealized exactly |
| Max drawdown | Largest fall of equity from its running peak, over the start, every sample, and the end |
| Maker spread capture | Σ (price − midpoint the aggressor met) × quantity, signed in the maker's favor |

The mark is the midpoint when both sides are quoted, otherwise the last trade, otherwise the scenario's reference price, and `metrics.json` and every sample say which. The simulated fundamental is never a mark. A statistic with nothing to measure is `null`, not zero. The ledger is posted from the exchange's executions as they occur; what an agent knows of its own fills still arrives after its report latency.

Rounding happens in three places, always to nearest with ties away from zero: a fee per fill, the value of a position at a half-tick midpoint when the tick size is odd, and reported means and ratios (suffixed `milli`, `ppm`, `ppb`, or `millibps`).

## Experiments

```bash
./build/release/core/markettwin experiment \
  --config configs/experiments/maker_comparison.json \
  --output data/experiments/maker_comparison
```

An experiment config names a base run config, a list of seeds (or `{"first": 1, "count": 200}`), and strategy variants. A variant replaces the policy of named agents and nothing else. Every variant runs every seed, on one thread, in a fixed order. The command prints a table to the terminal, writes its results to a **new** directory, and exits non-zero if any run failed.

| File | Contents |
| --- | --- |
| `experiment.json` | Engine version and build provenance, the canonical experiment config with the base config embedded, every run's status and digests, per-variant completed and failed counts, and the paired comparison; written last |
| `runs.jsonl` | One line per run: its summary and its full metrics document |
| `runs/<variant>/seed-<n>/` | A complete, replayable run directory per cell, when the config asks for `"artifacts": "All"` |

With `"artifacts": "None"` each run's digests are recorded instead. Any cell can be regenerated by running the experiment with that one seed and `"All"`, and `markettwin replay` then verifies it against the same digests.

### The maker comparison

Agent 3 of the baseline scenario is the subject. One variant gives it the baseline `MarketMaker`, the other the `InventoryMarketMaker`, with the same half spread, quote size, levels, spacing, wakeup schedule, latency, and limits. The other ten agents are identical in both. A run contains one subject, never both.

Within a seed, the two runs share everything exogenous: the fundamental value's path, every agent's wakeup times, the value traders' signal noise, and the noise traders' draws. They do not share a market history. The subject's quotes change who trades with whom, what the other agents then see, and so everything after. The comparison removes exogenous noise; it does not hold the rest of the market still.

Results of the shipped config: 200 seeds, 400 one-hour runs, all completed, engine 0.4.0. Money is in minor units with `tick_size_minor` 1, so 100 units is one currency unit at a reference price of 15,000 ticks. Differences are inventory-aware minus baseline, paired by seed.

| Metric for the subject | Baseline mean | Inventory-aware mean | Mean difference | Median difference | SD of differences | 95 % t interval | 95 % bootstrap interval |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Total P&L (minor) | −130,985.7 | −85,376.6 | +45,609.1 | +41,252.0 | 86,084.2 | [33,605.7, 57,612.5] | [33,723.0, 57,379.4] |
| Max drawdown (minor) | 167,480.0 | 100,824.9 | −66,655.1 | −63,034.0 | 78,767.9 | [−77,638.4, −55,671.8] | [−77,552.6, −56,056.1] |
| Maker spread capture (minor) | 103,191.1 | 100,595.5 | −2,595.6 | −2,501.0 | 8,792.9 | [−3,821.7, −1,369.5] | [−3,781.2, −1,372.1] |
| Mean absolute position (shares) | 1,070.5 | 223.4 | −847.2 | −838.6 | 159.3 | [−869.4, −824.9] | [−869.5, −825.7] |
| Final absolute position (shares) | 1,122.9 | 1,084.1 | −38.8 | −14.5 | 628.9 | [−126.5, 48.9] | [−125.8, 49.3] |
| Market mean spread (ticks) | 3.297 | 3.157 | −0.140 | −0.141 | 0.031 | [−0.144, −0.136] | [−0.144, −0.136] |

What this does and does not show:

- **Neither maker makes money in this market.** The baseline maker ends a run with a profit in 7 seeds of 200 and the inventory-aware one in 1 of 200. Both earn about 1,000 currency units of spread per run and lose more than that on the inventory they are left holding.
- The inventory-aware maker loses less on average (the interval for the difference excludes zero), with a smaller drawdown and about a fifth of the average inventory. It does better than the baseline in 142 seeds of 200 and worse in 58. Its best run is also far smaller than the baseline's best.
- Final inventory is not distinguishable between the two. The control's pull weakens as the horizon approaches, by construction.
- The intervals describe variation across seeds of one stylized model with one parameter setting. They are not evidence about real markets or about profitability, nineteen metrics are reported with no multiplicity correction, and no Sharpe ratio is reported because a run yields one terminal P&L, not a return series.

The full table, including the thirteen metrics not shown here, is what the command prints.

## Native exchange API

Include `markettwin/exchange/matching_engine.hpp` and link `markettwin::core`.
An engine represents one symbol and owns its book for its lifetime. All commands
are synchronous on one logical thread. Inside a run the kernel supplies arrival
metadata; the example below drives the exchange directly.

```cpp
using namespace markettwin;
MatchingEngine exchange{ExchangeConfig{
    .symbol = SymbolId{1}, .agents = {AgentId{1}, AgentId{2}}}};

Order ask{
    .id = OrderId{1}, .owner = AgentId{1}, .symbol = SymbolId{1},
    .side = Side::Sell, .type = OrderType::Limit, .limit_price = PriceTicks{100},
    .original_quantity = Quantity{10}, .remaining_quantity = Quantity{10},
    .tif = TimeInForce::Gtc, .arrival_time = SimTime{0}, .arrival_sequence = Sequence{1}};
auto accepted = exchange.submit(ask);

Order buy{
    .id = OrderId{2}, .owner = AgentId{2}, .symbol = SymbolId{1},
    .side = Side::Buy, .type = OrderType::Market, .limit_price = PriceTicks{0},
    .original_quantity = Quantity{4}, .remaining_quantity = Quantity{4},
    .tif = TimeInForce::Ioc, .arrival_time = SimTime{0}, .arrival_sequence = Sequence{2}};
auto filled = exchange.submit(buy); // Four shares at the resting price of 100 ticks.
auto depth = exchange.snapshot(5);  // Ask: six shares at 100 ticks.
auto canceled = exchange.cancel(OrderId{1}, AgentId{1});
exchange.verify_invariants();
```

`ExecutionReport::incoming` holds the final incoming status and cumulative filled,
canceled, and resting quantities. `trades` and `order_updates` preserve match order;
maker updates precede the final incoming update. Rejections carry `RejectReason`
and leave the exchange unchanged. Quotes and order lookup return `std::optional`.

[ADR 0001](docs/adr/0001-exchange-semantics.md) specifies self-trade rejection,
accepted-ID uniqueness, input validation, ordering, resource limits, and failure
semantics. Arrival sequences must increase and timestamps cannot regress across
accepted submissions. Prices are integer ticks and quantity units are shares/lots;
no currency scale or ledger arithmetic is implied by these records.

`markettwin::Simulation` is the run boundary for native callers: construct it with a
`RunConfig` and an optional output directory, then call `run()`. Without a directory
it runs in memory and returns the same summary and digests.

A new trading policy implements `ITradingAgent` (`markettwin/agents/agent.hpp`). It
receives an `AgentObservation` and emits intents through `AgentActions`; it has no
access to the book, the scheduler, or other agents. Registering it means adding its
parameters to `PolicyConfig`, the factory and bounds in `core/src/simulation/scenario.cpp`,
and its JSON form in `core/src/telemetry/scenario_codec.cpp`.

## Prerequisites

- CMake 3.25 or newer
- Ninja
- A C++20 compiler: Apple Clang, Clang, or GCC
- Network access on the first configure, so CMake can fetch the pinned nlohmann/json and GoogleTest archives

Optional, for formatting and static analysis:

- `clang-format` and `clang-tidy` (LLVM 18 or newer)

macOS:

```bash
brew install cmake ninja llvm
export PATH="$(brew --prefix llvm)/bin:$PATH"
```

Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y cmake ninja-build g++ clang-format clang-tidy
```

Check versions with `cmake --version`, `c++ --version`, and `clang-format --version`. `scripts/format.sh` and `scripts/lint.sh` search Homebrew LLVM and `/usr/lib/llvm-*` when the tools are not on `PATH`, and they exit with these install steps if the tools are missing.

## Build

Configure and build from the repository root. Builds stay out of source under `build/`.

```bash
cmake --preset debug
cmake --build --preset debug
```

Release:

```bash
cmake --preset release
cmake --build --preset release
```

Debug with AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
cmake --preset debug-sanitizers
cmake --build --preset debug-sanitizers
```

Without presets, the same Debug build is:

```bash
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/debug
```

`compile_commands.json` is written into the build directory.

The version command:

```bash
./build/debug/core/markettwin --version
```

Expected output:

```text
MarketTwin 0.3.0
```

`--help` prints usage. `run` and `replay` are described under [Running a simulation](#running-a-simulation) and `experiment` under [Experiments](#experiments). Any other argument exits with status 2.

## Tests

```bash
ctest --preset debug --output-on-failure
```

Equivalent after a manual configure:

```bash
ctest --test-dir build/debug --output-on-failure
```

Release and sanitizer presets have matching test presets (`release`, `debug-sanitizers`). Tests use GoogleTest and do not need a database, network service, or market data feed. Replay tests write to the system temporary directory and clean up after themselves. The suite includes two one-hour baseline runs (one in memory, one through the CLI with replay) and a small experiment through the CLI; a Debug build takes about 30 seconds in total.

Test layout:

- `core/tests/unit/exchange/` — matching, validation, snapshots, cancel lifecycle, and numeric bounds
- `core/tests/property/` — reference-model comparison, determinism, and per-order conservation after every action
- `core/tests/unit/simulation/` — scheduler ordering and bounds, strict config parsing for both schema versions, the fundamental process, the in-memory run boundary
- `core/tests/unit/agents/` — each policy against hand-built observations: quoting, inventory skew, warm-up, thresholds, limits, and random-draw alignment
- `core/tests/unit/common/` — SHA-256 standard vectors and accelerated/portable agreement; RNG published vectors, stream independence, and sampler distributions
- `core/tests/unit/accounting/` — checked money arithmetic at the 64-bit boundaries; opening, reducing, closing, and reversing long and short positions; partial fills, fees, duplicates, atomic rejection; the mark hierarchy, missing marks, and drawdown
- `core/tests/unit/telemetry/` — every market statistic against a small real exchange session, including empty and one-sided books and zero volume
- `core/tests/unit/experiment/` — means, medians, deviations, and intervals against hand-computed values; Student quantiles; bootstrap determinism
- `core/tests/replay/` — byte-identical reruns, corruption, truncation, tampering, and first-divergence reporting
- `core/tests/integration/` — scenario determinism, share conservation, canonical form of every record, latency and delivery boundaries, limits, shocks, scenario replay, golden summaries, and CLI run/replay tests; cash, share, and order-flow conservation, the cash constraint, config schema 3, accounting replay with first-divergence reporting; experiments: repeatability, paired exogenous processes, invalid designs, injected failures, output budgets, and a golden results document

The randomized tests print the seed and action number on failure. ASan/UBSan run
these same tests through the existing CI configuration.

## Benchmarks

Google Benchmark 1.9.4 is off by default, so a normal build does not download it. The target measures new-order, crossing, and cancel batches against the matching engine with fixture setup outside the timed region, ledger postings, account marking, paired statistics, and complete in-memory runs of the baseline scenario. CI runs it once as a smoke test and does not gate on timing.

```bash
cmake -S . -B build/benchmarks -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DMARKETTWIN_BUILD_BENCHMARKS=ON
cmake --build build/benchmarks --target markettwin_bench
./build/benchmarks/core/benchmarks/markettwin_bench
```

Stored baselines, with hardware, toolchain, workload, repetitions, and variance: [`docs/benchmarks/phase1b-baseline.md`](docs/benchmarks/phase1b-baseline.md) for the exchange [`docs/benchmarks/phase2a-baseline.md`](docs/benchmarks/phase2a-baseline.md) for scenario runs, and [`docs/benchmarks/phase2b-accounting.md`](docs/benchmarks/phase2b-accounting.md) for the cost of accounting against the Phase 2A binary. The figures are synthetic single-thread measurements, not a statement about live-exchange performance.

## Formatting and linting

```bash
scripts/format.sh          # fail if formatting differs
scripts/format.sh --fix    # rewrite sources
scripts/lint.sh build/debug
```

`scripts/lint.sh` reads `compile_commands.json` from the build directory you pass and checks the C++ sources listed there. Configure that build first. On macOS it passes the active SDK to Homebrew `clang-tidy`, which does not inherit Apple Clang's header lookup. The checked style is in `.clang-format`, `.clang-tidy`, and `.editorconfig`.

Naming used by the tidy config:

- Namespaces, functions, methods, variables: `snake_case`
- Types, structs, enums, enumerators: `PascalCase`
- Private members: `snake_case_`
- Files: `snake_case.hpp` / `snake_case.cpp`

## Layout

```text
architecture.md                 implementation contract
core/                           C++20 library, CLI, tests, benchmarks
bindings/python/                reserved for pybind11
python/markettwin/              reserved for the Python package
research/                       reserved for experiment notes and analysis
services/api/                   reserved for the research API
apps/web/                       reserved for the dashboard
infra/                          reserved for local deployment files
configs/                       run configurations (scenarios and the scripted demo)
configs/experiments/           experiment configurations
schemas/                       reserved for published schemas
docs/adr/                      accepted implementation decisions
docs/benchmarks/               stored benchmark baselines
data/                          generated run output, untracked
```

`research/` is the placeholder from the Phase 0 brief. The architecture places importable Python modules under `python/markettwin/`. Both directories are empty until that work starts.

## Dependencies

| Dependency | Pin | When |
| --- | --- | --- |
| CMake | >= 3.25 | Always |
| nlohmann/json | 3.11.3, SHA256 pinned in `cmake/Dependencies.cmake` | Always (private to the core library) |
| GoogleTest | 1.15.2, SHA256 pinned in `cmake/Dependencies.cmake` | Tests, the default |
| Google Benchmark | 1.9.4, SHA256 pinned in `cmake/Dependencies.cmake` | `MARKETTWIN_BUILD_BENCHMARKS=ON` |

No package manager manifest beyond CMake FetchContent is required. pybind11, FastAPI, PostgreSQL, Redis, and frontend packages are intentionally absent.

Useful CMake options:

| Option | Default | Purpose |
| --- | --- | --- |
| `MARKETTWIN_BUILD_TESTS` | `ON` | GoogleTest and CTest |
| `MARKETTWIN_BUILD_BENCHMARKS` | `OFF` | Google Benchmark exchange target |
| `MARKETTWIN_ENABLE_SANITIZERS` | `OFF` | ASan and UBSan on MarketTwin targets |
| `MARKETTWIN_WARNINGS_AS_ERRORS` | `ON` | `-Werror` / `/WX` for MarketTwin targets |

## Phases

Status follows [`architecture.md`](architecture.md) section 20. Phases 1A, 1B, 2A, and 2B are implemented; Phase 3A is next.

| Phase | Scope | State |
| --- | --- | --- |
| 0 | CMake, CI, formatter, test runner, version CLI, no-op simulation type | Present in this tree |
| 1A | Order-book semantics and matching invariants | Implemented and locally verified |
| 1B | Event kernel, canonical trace, exchange replay | Implemented and locally verified |
| 2A | Trading agents, latency, shocks | Implemented and locally verified |
| 2B | Accounting, artifacts, paired experiments | Implemented and locally verified |
| 3A | pybind11, API, worker, dashboard baseline | Not started |
| 3B | AI research tools, replay UI, end-to-end polish | Not started |
| 4 | Stretch work after the exit gates | Not started |

## License

No license file has been added yet.
