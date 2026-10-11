# Phase 4A historical pipeline baseline

Measured 2026-10-11 on an Apple M1 Pro, macOS 26.6, Apple Clang 21.0.0, arm64.
The native extension was the service's local debug build. This is a single smoke
measurement, not a throughput guarantee.

Command:

```bash
cd services/api
uv run python scripts/historical_benchmark.py --records 50000
```

The harness generates 50,000 fabricated alternating L1 quote/trade messages with unique
UTC-nanosecond timestamps and provider sequences. Fixture generation is outside the timed
region.

| Measurement | Result |
| --- | ---: |
| Native normalize, validate, sort, write | 1.076018 s |
| Normalization throughput | 46,468 records/s |
| Digest and deterministic playback verification | 0.515270 s |
| Verification throughput | 97,037 events/s |
| Completed artifact size | 37,285,669 bytes |
| Process peak resident set | 148,537,344 bytes |
| Mean service read, first 500 samples (100 repetitions) | 1.572052 ms |

The output verified successfully. Peak RSS is process-wide and includes Python, the
service/SQLite registry, the extension, generated-record handling, and allocator
high-water state; it is not isolated native heap use. The current importer is bounded by
500,000 records and 256 MB of raw input but sorts its bounded event vector in memory.
HTTP event and series reads are paginated to at most 500 records. Browser playback renders
a 500-observation window.

Indexed seek latency and browser frame timing were not separately instrumented in this
baseline; playback seeks by array index in the loaded window. Alpaca network throughput
was not measured because credentials and feed entitlement were unavailable.
