# Contributing to MarketTwin

MarketTwin is a C++20-first synthetic equity exchange and research platform. The matching engine, order book, event scheduler, simulation clock, trading agents, ledger, and experiment statistics belong in C++. Python, the API, and the dashboard sit outside a run.

`architecture.md` is the implementation contract. Read sections 1–12 and the current phase before changing core behavior. This repository has completed **Phase 2B**: the native exchange, the logical-time kernel, the canonical command/execution trace, replay, trading agents, scenario processes, the ledger, market statistics, and paired experiments. The next scope is Phase 3A (bindings, API, and dashboard baseline). Keep services and deployment infrastructure in their assigned later phases. Exchange behavior is specified in `docs/adr/0001-exchange-semantics.md`; ordering, trace format, and replay in `docs/adr/0002-kernel-trace-and-replay.md`; agents, random streams, latency, and scenarios in `docs/adr/0003-agents-rng-latency-and-scenarios.md`; the ledger, marks, market statistics, and experiments in `docs/adr/0004-ledger-marks-metrics-and-experiments.md`.

## Conventions

- C++20, out-of-source CMake, and the `markettwin::core` library target.
- File names are `snake_case`. Types and enumerators are `PascalCase`. Functions, methods, and variables are `snake_case`. Private data members use a trailing underscore.
- One simulation run stays deterministic and logically single-threaded. Do not schedule matching across threads, and do not call Python per event.
- Integer ticks, explicit seeds, and stable event order are part of the contract. Do not introduce floating point or `std::` random distributions anywhere in the engine, the ledger, or the experiment statistics: golden digests are held across toolchains because every calculation is integer.
- Money is `Money` (minor units) and has no arithmetic operators. Use the checked helpers in `markettwin/accounting/money.hpp`; they throw on overflow. Round only where ADR 0004 says a value leaves exact arithmetic, and to nearest with ties away from zero.
- The ledger and the market statistics only read what the exchange returns. They must never schedule an event, draw a random number, consume a trace sequence, or be readable by a policy.
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

A change should name the tests it adds and the commands that were run. A change to any canonical record, digest input, ordering rule, sampler, or policy changes a summary under `core/tests/replay/golden/`; that needs a schema or engine version decision and an ADR, not a quiet fixture refresh. Retuning a shipped scenario config changes `phase2a_baseline_summary.json` and `phase2b_baseline_metrics.json` and should say so. A change to an accounting formula, a rounding rule, a metric definition, or a statistic changes `phase2b_baseline_metrics.json` or `phase2b_experiment_results.json` and needs an accounting or metrics schema version decision in the same way. Generated run output under `data/` and secrets stay untracked. Leave future-phase directories empty until that phase starts.
