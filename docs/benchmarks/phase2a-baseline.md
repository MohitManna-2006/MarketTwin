# Phase 2A scenario benchmark baseline

Recorded 2026-10-10. Raw Google Benchmark output: [`phase2a-baseline.json`](phase2a-baseline.json).

These numbers are a regression reference for this code on this machine. They are
synthetic single-threaded runs of a stylized scenario and say nothing about
live-exchange capability.

## Environment

| Item | Value |
| --- | --- |
| Hardware | Apple M1 Pro, 8 cores, 16 GB |
| OS | macOS 26.6 (arm64) |
| Compiler | Apple clang 21.0.0 (clang-2100.1.1.101) |
| Build | `CMAKE_BUILD_TYPE=Release` (`-O3 -DNDEBUG`), no sanitizers |
| Harness | Google Benchmark 1.9.4 |
| Source | Phase 2A working tree on top of `c4ad3bb` (engine version 0.3.0) |
| Machine state | Not quiesced: load average about 5 at the start of the run |

## Workload

`core/benchmarks/simulation_bench.cpp` runs `configs/baseline.json` (eleven agents, seed
42) in memory through `run_kernel` with a null sink, with the horizon shortened to the
stated number of simulated seconds. Every canonical record is still built and hashed, so
the summary digests are the real ones; only the file writes are absent. Config loading is
outside the timed region. An item is one dispatched kernel event: an exchange command, a
wakeup, a delivery, a fundamental tick, or a sample.

| Benchmark | Simulated time | Events per run | Trades per run |
| --- | ---: | ---: | ---: |
| `baseline_scenario/300` | 5 minutes | about 24,000 | 659 |
| `baseline_scenario/3600` | 1 hour | 298,798 | 8,205 |

## Results

10 repetitions, `--benchmark_min_time=0.3s`. Statistics are across repetitions of CPU time
per run.

| Benchmark | Median CPU / run | Mean CPU / run | Std dev | CV | Events / s (mean) |
| --- | ---: | ---: | ---: | ---: | ---: |
| `baseline_scenario/300` | 26.7 ms | 26.7 ms | 0.05 ms | 0.2 % | 901 k |
| `baseline_scenario/3600` | 327 ms | 328 ms | 0.9 ms | 0.3 % | 912 k |

One simulated hour therefore costs about a third of a second of CPU, roughly 11,000
times faster than the clock it simulates. Through the CLI with all four artifacts written
(about 45 MB of logs), the same hour takes about 0.4 s of CPU.

The exchange microbenchmarks from the Phase 1B baseline were re-run in the same session
and are unchanged within noise: 96 ns, 152 ns, and 45 ns per operation for new-order,
crossing, and cancel.

## Where the time goes

A sampling profile of the one-hour run before optimization attributed about 58 % of CPU
to SHA-256, about 20 % to building and serializing JSON trees, and under 5 % to the
scheduler, the matching engine, and all agent logic combined. Trace integrity, not
simulation, was the cost. Two changes followed (ADR 0003); neither changes any output
byte:

| Change | CPU for a one-hour CLI run |
| --- | ---: |
| Before | 0.95 s |
| Block-wise SHA-256 with ARMv8 SHA-2 instructions | 0.47 s |
| Envelopes, trades, and order statuses written as canonical text | 0.34 s |

Those three figures are single `time` measurements of the CLI on one config taken during
development, before the scenario's final calibration, so compare them with each other and
not with the table above. After both changes the profile is spread across hashing, JSON
for exchange commands, and allocation, with no single dominant cost.

On Linux aarch64 with GCC 14 in a container on the same machine, where the build does not
target the SHA-2 instructions and the portable hash is used, the one-hour in-memory run
took about 0.8 s in a single smoke iteration.

## Reproduce

```bash
cmake -S . -B build/benchmarks -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DMARKETTWIN_BUILD_BENCHMARKS=ON
cmake --build build/benchmarks --target markettwin_bench
./build/benchmarks/core/benchmarks/markettwin_bench \
  --benchmark_repetitions=10 --benchmark_min_time=0.3s \
  --benchmark_report_aggregates_only=true
```

CI runs the same binary once as a smoke test with no timing threshold.
