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
C++20 simulation                      (in progress: library skeleton only)
        one logical thread per run
        event scheduler, matching engine, agents, accounting, telemetry
```

Planned native boundaries, not yet implemented:

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

**Phase 0 — repository foundation.** The CMake build, `markettwin_core`, a version CLI, GoogleTest, formatter config, and GitHub Actions CI are in place.

Implemented now:

- C++20 library target `markettwin::core`
- CLI that prints the engine version
- No-op `markettwin::Simulation` used to lock the future run boundary and prove linkage
- GoogleTest suite and CTest, including CLI checks
- Optional AddressSanitizer and UndefinedBehaviorSanitizer build
- Optional Google Benchmark target, off unless requested

Not implemented:

- Order book, matching, agents, accounting, scenarios, or replay
- Python bindings, FastAPI, PostgreSQL, Redis, or the React app
- Experiment comparison, AI research tools, or artifact export

`markettwin::Simulation::run()` returns a completed summary with zero events. It does not simulate a market.

## Prerequisites

- CMake 3.25 or newer
- Ninja
- A C++20 compiler: Apple Clang, Clang, or GCC
- Network access on the first configure, so CMake can fetch the pinned GoogleTest archive

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
MarketTwin 0.1.0
```

`--help` prints usage. Any other argument exits non-zero. The CLI does not submit orders or start a run.

## Tests

```bash
ctest --preset debug --output-on-failure
```

Equivalent after a manual configure:

```bash
ctest --test-dir build/debug --output-on-failure
```

Release and sanitizer presets have matching test presets (`release`, `debug-sanitizers`). Tests use GoogleTest and do not need a database, network service, or market data feed. GoogleTest 1.15.2 is fetched by CMake from a pinned tarball.

Future test directories, reserved and empty for now:

- `core/tests/unit/exchange/` — matching and order validation
- `core/tests/unit/simulation/` — scheduler and kernel
- `core/tests/integration/` — cross-subsystem runs
- `core/tests/property/` — randomized invariant traces
- `core/tests/replay/` — digest and divergence checks

## Benchmarks

Google Benchmark 1.9.4 is wired but off by default, so a normal build does not download it. Enable it for a harness smoke test of the no-op run. This is not a matching-engine benchmark, and CI does not gate on timing.

```bash
cmake -S . -B build/benchmarks -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DMARKETTWIN_BUILD_BENCHMARKS=ON
cmake --build build/benchmarks --target markettwin_bench
./build/benchmarks/core/benchmarks/markettwin_bench
```

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
configs/  schemas/  docs/adr/   reserved for scenarios, schemas, and ADRs
```

`research/` is the placeholder from the Phase 0 brief. The architecture places importable Python modules under `python/markettwin/`. Both directories are empty until that work starts.

## Dependencies

| Dependency | Pin | When |
| --- | --- | --- |
| CMake | >= 3.25 | Always |
| GoogleTest | 1.15.2, SHA256 pinned in `cmake/Dependencies.cmake` | Tests, the default |
| Google Benchmark | 1.9.4, SHA256 pinned in `cmake/Dependencies.cmake` | `MARKETTWIN_BUILD_BENCHMARKS=ON` |

No package manager manifest beyond CMake FetchContent is required for Phase 0. pybind11, FastAPI, PostgreSQL, Redis, and frontend packages are intentionally absent.

Useful CMake options:

| Option | Default | Purpose |
| --- | --- | --- |
| `MARKETTWIN_BUILD_TESTS` | `ON` | GoogleTest and CTest |
| `MARKETTWIN_BUILD_BENCHMARKS` | `OFF` | Google Benchmark smoke target |
| `MARKETTWIN_ENABLE_SANITIZERS` | `OFF` | ASan and UBSan on MarketTwin targets |
| `MARKETTWIN_WARNINGS_AS_ERRORS` | `ON` | `-Werror` / `/WX` for MarketTwin targets |

## Phases

Status follows [`architecture.md`](architecture.md) section 20. Only Phase 0 is started.

| Phase | Scope | State |
| --- | --- | --- |
| 0 | CMake, CI, formatter, test runner, version CLI, no-op simulation type | Present in this tree |
| 1A | Order-book semantics and matching invariants | Not started |
| 1B | Event kernel, canonical trace, exchange replay | Not started |
| 2A | Trading agents, latency, shocks | Not started |
| 2B | Accounting, artifacts, paired experiments | Not started |
| 3A | pybind11, API, worker, dashboard baseline | Not started |
| 3B | AI research tools, replay UI, end-to-end polish | Not started |
| 4 | Stretch work after the exit gates | Not started |

## License

No license file has been added yet.
