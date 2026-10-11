# MarketTwin

Digital twin for market simulation.

MarketTwin is a research platform for studying how independent trading strategies interact inside a synthetic continuous double-auction equity exchange. The market-critical path is **C++20**: order matching, logical time, trading agents, accounting, and deterministic event replay will live in the native core. Python will orchestrate research and AI workflows. A React and TypeScript dashboard will display results. None of those later surfaces are allowed to compute authoritative prices, fills, or P&L.

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
C++20 simulation                      (implemented: exchange, kernel, agents, scenarios, trace, replay)
        one logical thread per run
        event scheduler, matching engine, agents, accounting, telemetry
```

Subsystem boundaries (exchange, simulation kernel, agents, and telemetry implemented; later subsystems planned):

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

**Phase 2A — trading agents and scenario processes.** Five trading policies run inside the native kernel against a synthetic fundamental value, each seeing only the market data and order reports that have been delivered to it after its own latency. A one-hour market of eleven agents produces about 8,000 trades with a two-sided book throughout, takes about a third of a second of CPU on an M1 Pro, and replays to identical digests.

Implemented now:

- C++20 library target `markettwin::core` and the `markettwin` CLI (`run`, `replay`, `--version`)
- Strong integer price, quantity, identity, timestamp, and sequence types
- Single-symbol `MatchingEngine` with ordered bid/ask maps, FIFO lists, and an ID index
- Limit GTC/IOC and market IOC orders, partial fills, and residual cancellation
- Structured execution/status reports, transactional rejection, and self-trade prevention
- Immutable depth snapshots, optional best quotes, and full book/index invariant checks
- Heap scheduler ordered by `(timestamp, scheduler sequence)` with an inclusive horizon, event and queue caps, and recorded stop reasons
- Strict versioned JSON run configs: unknown or missing fields, duplicate keys, floats, and out-of-range numbers are rejected
- Canonical `config.json`, `commands.jsonl`, `trace.jsonl`, and a `manifest.json` with SHA-256 digests, published only after a completed run
- Replay that regenerates the run from its config and seed, byte-checks every command before dispatch and every output after it, and checks final book and trade digests
- Trading policies: baseline market maker, inventory-aware market maker, momentum, value, and noise
- A delivered-observation boundary: per-agent order-entry, report, and market-data latency, with per-agent position, order-size, open-order, and message limits
- xoshiro256\*\* random streams per consumer with integer-only samplers, so a seed gives the same run on every toolchain
- A mean-reverting synthetic fundamental value and three shocks: fundamental jump, liquidity withdrawal, and volatility regime
- Unit tests, 32 seeded randomized exchange traces checked against an independent reference matcher, policy and scenario tests, replay corruption tests, and golden digest fixtures
- Optional AddressSanitizer and UndefinedBehaviorSanitizer build
- Optional Google Benchmark target for exchange operations and complete scenario runs

Not implemented:

- Cash, P&L, fees, or any ledger: agents and summaries track shares only
- Order-flow imbalance, latency degradation, and quote-withdrawal shocks
- Parquet artifacts or experiment comparison
- Python bindings, FastAPI, PostgreSQL, Redis, or the React app
- AI research tools

The scenarios are stylized. Prices come from the interaction of the configured agents, not from market data, and nothing here is calibrated to a real stock or says anything about real-world profitability.

## Running a simulation

```bash
./build/release/core/markettwin run --config configs/baseline.json --output data/baseline
./build/release/core/markettwin replay --input data/baseline
```

`run` prints the terminal summary as one JSON line and writes four files into a **new** directory. It refuses an existing directory, so a run is never overwritten or appended to.

| File | Contents |
| --- | --- |
| `config.json` | Canonical form of the input config; its digest is linked from both logs |
| `commands.jsonl` | Header, then each dispatched command in dispatch order (inputs to the exchange) |
| `trace.jsonl` | Header, then trades, order statuses, cancel results, snapshots, fundamental updates, shock transitions, and the terminal record |
| `manifest.json` | Engine version, build provenance, terminal summary, and artifact checksums; written last |

`replay` reruns the simulation from `config.json`, including every agent decision, requires each command to match `commands.jsonl` byte for byte before it reaches a fresh exchange, and compares each output with `trace.jsonl`. It exits 0 and prints `"verified":true` when everything agrees, including the final book digest, trade digest, and file checksums. On the first difference it exits 1 and reports the trace sequence, the expected and actual records, the causal command, and the last verified output. A run that failed part-way has no manifest and does not verify.

### Scenario configs

| Config | What it runs |
| --- | --- |
| `configs/baseline.json` | One simulated hour: two baseline makers, one inventory-aware maker, two value traders, two momentum traders, four noise traders |
| `configs/fundamental_shock.json` | The baseline with the fundamental dropping 150 ticks at 30 minutes |
| `configs/liquidity_shock.json` | The baseline with both baseline makers withdrawn from minute 20 to minute 30 |
| `configs/volatility_shock.json` | The baseline with fundamental volatility quadrupled from minute 20 to minute 40 |
| `configs/phase1b_demo.json` | A scripted run with no agents: every order, cancel, snapshot, and stop comes from the config |

The four scenario configs share a seed, so until its shock begins each variant is the same market as the baseline.

A scenario config (`schema_version` 2) is a scripted config plus a `scenario` object: a public reference price, the fundamental process, optional periodic depth sampling, the agents, and the shocks. Each agent has an ID registered with the exchange, a wakeup schedule (fixed interval or exponential), three latencies, hard limits, and a policy with its parameters. All values are integers: prices in ticks, sub-tick values in milliticks, times in nanoseconds, and fractions in parts per million. Malformed or out-of-range configs fail before the run starts.

The summary's `scenario` object reports, per agent, wakeups, orders and cancels sent, intents refused by the agent's own limits, fills, filled quantity, and net position in shares, along with the final fundamental value and a digest of every random stream's final state.

`configs/phase1b_demo.json` remains the reference scripted example: out-of-order scheduling, same-timestamp commands, a multi-level fill, a rejected foreign cancel, IOC residuals, an invalid order, and an explicit stop that leaves a later same-time order undispatched. Orders that are well-formed but invalid for the exchange, such as zero quantity or an unknown owner, run and are logged as rejections. The kernel assigns arrival time and sequence; configs cannot.

Golden summaries for the scripted demo and the baseline scenario are pinned in `core/tests/replay/golden/` and hold across Apple Clang and GCC builds.

[ADR 0002](docs/adr/0002-kernel-trace-and-replay.md) specifies ordering, stop conditions, the record format, digests, and replay failure codes. [ADR 0003](docs/adr/0003-agents-rng-latency-and-scenarios.md) specifies the agents, what they can observe, latency, random streams, the fundamental process, and shocks.

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

`--help` prints usage. `run` and `replay` are described under [Running a simulation](#running-a-simulation). Any other argument exits with status 2.

## Tests

```bash
ctest --preset debug --output-on-failure
```

Equivalent after a manual configure:

```bash
ctest --test-dir build/debug --output-on-failure
```

Release and sanitizer presets have matching test presets (`release`, `debug-sanitizers`). Tests use GoogleTest and do not need a database, network service, or market data feed. Replay tests write to the system temporary directory and clean up after themselves. The suite includes two one-hour baseline runs (one in memory, one through the CLI with replay); a Debug build takes about 20 seconds in total.

Test layout:

- `core/tests/unit/exchange/` — matching, validation, snapshots, cancel lifecycle, and numeric bounds
- `core/tests/property/` — reference-model comparison, determinism, and per-order conservation after every action
- `core/tests/unit/simulation/` — scheduler ordering and bounds, strict config parsing for both schema versions, the fundamental process, the in-memory run boundary
- `core/tests/unit/agents/` — each policy against hand-built observations: quoting, inventory skew, warm-up, thresholds, limits, and random-draw alignment
- `core/tests/unit/common/` — SHA-256 standard vectors and accelerated/portable agreement; RNG published vectors, stream independence, and sampler distributions
- `core/tests/replay/` — byte-identical reruns, corruption, truncation, tampering, and first-divergence reporting
- `core/tests/integration/` — scenario determinism, share conservation, canonical form of every record, latency and delivery boundaries, limits, shocks, scenario replay, golden summaries, and CLI run/replay tests

The randomized tests print the seed and action number on failure. ASan/UBSan run
these same tests through the existing CI configuration.

## Benchmarks

Google Benchmark 1.9.4 is off by default, so a normal build does not download it. The target measures new-order, crossing, and cancel batches against the matching engine with fixture setup outside the timed region, and complete in-memory runs of the baseline scenario. CI runs it once as a smoke test and does not gate on timing.

```bash
cmake -S . -B build/benchmarks -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DMARKETTWIN_BUILD_BENCHMARKS=ON
cmake --build build/benchmarks --target markettwin_bench
./build/benchmarks/core/benchmarks/markettwin_bench
```

Stored baselines, with hardware, toolchain, workload, repetitions, and variance: [`docs/benchmarks/phase1b-baseline.md`](docs/benchmarks/phase1b-baseline.md) for the exchange and [`docs/benchmarks/phase2a-baseline.md`](docs/benchmarks/phase2a-baseline.md) for scenario runs. The figures are synthetic single-thread measurements, not a statement about live-exchange performance.

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

Status follows [`architecture.md`](architecture.md) section 20. Phases 1A, 1B, and 2A are implemented; Phase 2B is next.

| Phase | Scope | State |
| --- | --- | --- |
| 0 | CMake, CI, formatter, test runner, version CLI, no-op simulation type | Present in this tree |
| 1A | Order-book semantics and matching invariants | Implemented and locally verified |
| 1B | Event kernel, canonical trace, exchange replay | Implemented and locally verified |
| 2A | Trading agents, latency, shocks | Implemented and locally verified |
| 2B | Accounting, artifacts, paired experiments | Not started |
| 3A | pybind11, API, worker, dashboard baseline | Not started |
| 3B | AI research tools, replay UI, end-to-end polish | Not started |
| 4 | Stretch work after the exit gates | Not started |

## License

No license file has been added yet.
