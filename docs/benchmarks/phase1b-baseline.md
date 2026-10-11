# Phase 1B exchange benchmark baseline

Recorded 2026-10-10. Raw Google Benchmark output: [`phase1b-baseline.json`](phase1b-baseline.json).

These numbers are a regression reference for this code on this machine. They are
synthetic single-threaded microbenchmarks of the matching engine alone and say nothing
about live-exchange capability, end-to-end simulation throughput, or trace I/O cost.

## Environment

| Item | Value |
| --- | --- |
| Hardware | Apple M1 Pro, 8 cores, 16 GB |
| OS | macOS 26.6 (arm64) |
| Compiler | Apple clang 21.0.0 (clang-2100.1.1.101) |
| Build | `CMAKE_BUILD_TYPE=Release` (`-O3 -DNDEBUG`), no sanitizers |
| Harness | Google Benchmark 1.9.4 |
| Source | Phase 1B working tree on top of `c7eec34` (engine version 0.2.0) |
| Machine state | Not quiesced: load average about 13–19 during the run |

## Workload

`core/benchmarks/exchange_bench.cpp`. Each iteration times a batch of 512 operations
against a single-symbol `MatchingEngine` with two agents. Engine construction, book
seeding, and teardown run with timing paused.

| Benchmark | Timed operations | Book before the batch |
| --- | --- | --- |
| `new_order` | 512 non-crossing GTC limit buys of 1 share at one price | Empty |
| `crossing` | 512 GTC limit buys of 1 share, each fully filling one resting sell | 512 resting 1-share sells at one price |
| `cancel` | 512 owner cancels in FIFO order | 512 resting 1-share sells at one price |

All orders sit on a single price level, so these measure the shallow-book fast path:
index insert/erase, FIFO list maintenance, and report construction. Deep books, multi-level
sweeps, and mixed flows are not covered yet.

## Results

20 repetitions, `--benchmark_min_time=0.2s`. Statistics are across repetitions of the
per-batch CPU time; per-operation figures divide by 512.

| Benchmark | Median CPU / batch | Mean CPU / batch | Std dev | CV | Median / operation | Operations / s (mean) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `new_order` | 48.9 µs | 49.4 µs | 1.09 µs | 2.2 % | 96 ns | 10.4 M |
| `crossing` | 78.4 µs | 78.9 µs | 1.43 µs | 1.8 % | 153 ns | 6.5 M |
| `cancel` | 23.7 µs | 23.9 µs | 0.49 µs | 2.0 % | 46 ns | 21.5 M |

Wall-clock time was far noisier than CPU time (CV 18–50 %) because the machine was busy.
Compare against the CPU columns, and treat differences under roughly 5 % as noise.

## Reproduce

```bash
cmake -S . -B build/benchmarks -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DMARKETTWIN_BUILD_BENCHMARKS=ON
cmake --build build/benchmarks --target markettwin_bench
./build/benchmarks/core/benchmarks/markettwin_bench \
  --benchmark_repetitions=20 --benchmark_min_time=0.2s \
  --benchmark_report_aggregates_only=true
```

CI runs the same binary once as a smoke test with no timing threshold.
