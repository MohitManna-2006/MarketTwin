# Contributing to MarketTwin

MarketTwin is a C++20-first synthetic equity exchange and research platform. The matching engine, order book, event scheduler, simulation clock, and trading agents belong in C++. Python, the API, and the dashboard sit outside a run.

`architecture.md` is the implementation contract. Read sections 1–12 and the current phase before changing core behavior. This repository has completed **Phase 2A**: the native exchange, the logical-time kernel, the canonical command/execution trace, replay, trading agents, and scenario processes. The next scope is Phase 2B (accounting, artifacts, and paired experiments). Keep services and deployment infrastructure in their assigned later phases. Exchange behavior is specified in `docs/adr/0001-exchange-semantics.md`; ordering, trace format, and replay in `docs/adr/0002-kernel-trace-and-replay.md`; agents, random streams, latency, and scenarios in `docs/adr/0003-agents-rng-latency-and-scenarios.md`.

## Conventions

- C++20, out-of-source CMake, and the `markettwin::core` library target.
- File names are `snake_case`. Types and enumerators are `PascalCase`. Functions, methods, and variables are `snake_case`. Private data members use a trailing underscore.
- One simulation run stays deterministic and logically single-threaded. Do not schedule matching across threads, and do not call Python per event.
- Integer ticks, explicit seeds, and stable event order are part of the contract. Do not introduce floating point or `std::` random distributions anywhere in the engine: golden digests are held across toolchains because every calculation is integer.
- Random draws come from a stream owned by the consumer (`markettwin/common/rng.hpp`). A policy draws the same number of variates on every callback whatever the market looks like, so one agent's change cannot shift another's random inputs.
- Policies see only their `AgentObservation`. Do not pass the book, the scheduler, the fundamental, or another agent's state into a policy.
- Warnings are errors on MarketTwin targets. Keep GoogleTest and Google Benchmark includes out of that policy; they are fetched as pinned third-party sources.
- Architecture changes need a dated ADR in `docs/adr/` and a matching update to `architecture.md`. Do not silently replace a specified algorithm, ordering rule, or schema.

## Local checks

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
scripts/format.sh
scripts/lint.sh build/debug
```

`scripts/format.sh --fix` rewrites formatting. `scripts/lint.sh` needs `compile_commands.json` from a configured build. If `clang-format` or `clang-tidy` is missing, the script prints the install command and exits non-zero.

## Review expectations

A change should name the tests it adds and the commands that were run. A change to any canonical record, digest input, ordering rule, sampler, or policy changes a summary under `core/tests/replay/golden/`; that needs a schema or engine version decision and an ADR, not a quiet fixture refresh. Retuning a shipped scenario config changes `phase2a_baseline_summary.json` and should say so. Generated run output under `data/` and secrets stay untracked. Leave future-phase directories empty until that phase starts.
