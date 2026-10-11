# Phase 2B accounting overhead

Recorded 2026-10-10. Raw Google Benchmark output: [`phase2b-accounting.json`](phase2b-accounting.json).

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
| Phase 2A binary | The benchmark binary built from the Phase 2A tree before this work began; it reports engine 0.3.0 |
| Phase 2B binary | Engine 0.4.0, Phase 2B working tree on top of `351111e` |
| Machine state | **Not quiesced.** A video call and other applications were running, and the reported load average was about 100. Use the CPU-time columns; wall-clock times in the raw file are noisier |

## Method

The Phase 2A benchmark binary was kept, and the two binaries were run alternately, three
rounds each, in one session: `--benchmark_repetitions=10 --benchmark_min_time=0.3s`. Both
run the same workload: `configs/baseline.json` (eleven agents, seed 42) in memory through
`run_kernel` with a null sink, with every canonical record built and hashed. The two
produce the same market: 298,798 events and 8,205 trades per simulated hour, and the same
book, trade, and RNG digests.

What the Phase 2B binary does in addition, per simulated hour: 8,205 ledger postings with
a record each, 3,600 time-series samples that mark all eleven accounts, two top-of-book
reads per order, exact time-in-state bookkeeping after every command, and the final
metrics document. That is 6.1 MB of accounting records built and hashed beside 49.2 MB of
command and execution records.

## Complete scenario runs

Median CPU time per run, one figure per round.

| Benchmark | Phase 2A (0.3.0) | Phase 2B (0.4.0) | Mean of medians | Change |
| --- | --- | --- | --- | ---: |
| `baseline_scenario/300` (5 simulated minutes) | 27.1, 27.0, 27.0 ms | 29.6, 29.4, 29.3 ms | 27.0 → 29.4 ms | +8.9 % |
| `baseline_scenario/3600` (1 simulated hour) | 336, 332, 330 ms | 356, 358, 367 ms | 333 → 360 ms | +8.3 % |

The coefficient of variation across the ten repetitions of a round was between 0.3 % and
2.9 %, so the difference between the binaries is several times the noise within either.
One simulated hour with full accounting costs about 28 ms more CPU than without, and
events per second fall from about 898 k to about 829 k.

A sampling profile of the Phase 2B run shows no accounting function near the top. The
ledger and the valuation arithmetic are a fraction of a millisecond per hour (below); the
added time is building and hashing the extra records, spread across SHA-256, string
appends, and allocation.

## Accounting operations in isolation

Ten repetitions, median CPU time, from the Phase 2B binary.

| Benchmark | What one item is | Median CPU | Per item | CV |
| --- | --- | ---: | ---: | ---: |
| `ledger_apply` | One trade booked into both accounts, with fees, basis, and realized P&L (batch of 4,096 across eight agents; positions open, shrink, close, and reverse) | 363 µs per batch | 89 ns | 1.2 % |
| `mark_accounts` | One account marked to a midpoint (batch of eight) | 64 ns per batch | 8 ns | 1.1 % |
| `paired_statistics/200` | Summary, Student t interval, and a 10,000-resample bootstrap over 200 paired differences | 13.9 ms | 13.9 ms | 2.0 % |

At 8,205 trades and 39,600 account marks per simulated hour, the ledger and valuation
arithmetic together come to about one millisecond of the 28.

The exchange microbenchmarks were re-run in the same session with both binaries and are
unchanged within noise: 96, 158, and 47 ns per operation for new-order, crossing, and
cancel in Phase 2B, against 100, 157, and 46 ns in Phase 2A.

## Experiments

`configs/experiments/maker_comparison.json` (400 one-hour runs in memory, then 19 paired
comparisons with bootstrap intervals) took 2 minutes 29 seconds of wall-clock time and
141 seconds of CPU as a single `time` measurement of the CLI, on the same machine while a
lint job was running. Runs execute one after another on one thread.

## Linux

On Linux aarch64 with GCC 14.2 in a container on the same machine, where the build does
not target the SHA-2 instructions and the portable hash is used, a single smoke iteration
gave 868 ms for `baseline_scenario/3600`, 438 µs for `ledger_apply`, and 16.3 ms for
`paired_statistics/200`. The Phase 2A figure for the first was about 0.8 s.

## Reproduce

```bash
cmake -S . -B build/benchmarks -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DMARKETTWIN_BUILD_BENCHMARKS=ON
cmake --build build/benchmarks --target markettwin_bench
./build/benchmarks/core/benchmarks/markettwin_bench \
  --benchmark_repetitions=10 --benchmark_min_time=0.3s \
  --benchmark_report_aggregates_only=true
```

To compare against Phase 2A, build the same target from commit `c13f106` in a separate
build directory and alternate the two binaries. CI runs the binary once as a smoke test
with no timing threshold.
