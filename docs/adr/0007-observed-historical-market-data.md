# ADR 0007: Observed historical equity data

**Date:** 2026-10-11  
**Status:** Accepted for Phase 4A

## Boundary

Historical data is an observed-market subsystem, not another simulation scenario.
Provider trades are reported prints, not executions of MarketTwin agents. Provider quotes
are observations and never become resting orders in `MatchingEngine`. They imply neither
accessible liquidity nor a hypothetical fill. Phase 4A contains no brokerage, paper
execution, strategy P&L, market-impact estimate, or live stream.

The implementation boundary is:

```text
local fixture or Alpaca REST -> Python acquisition -> immutable raw bytes
  -> one coarse C++ normalization call -> canonical historical session
  -> deterministic C++ observation playback -> FastAPI -> React microscope
```

Python owns authentication, pagination, bounded retries, entitlement errors, and durable
job orchestration. C++ owns decimal conversion, UTC timestamp conversion, validation,
ordering, canonical records, observations, metrics, and SHA-256 verification. Python is
never called per record.

## Native schema and ordering

Schema 1 supports reported trades and L1 quotes. L2/L3 are intentionally unsupported;
top-of-book records are never presented as reconstructed depth.
Provider correction/cancellation messages and receive timestamps are preserved only when
a future verified adapter/schema explicitly maps them; schema 1 rejects unsupported event
kinds instead of guessing their effect.

Prices are decimal strings at acquisition and checked into signed 64-bit millionths of a
US dollar. More than six decimal places, overflow, signs, and invalid/zero values are
rejected; no synthetic tick grid is involved. The source decimal is retained beside the
normalized integer. RFC3339 timestamps must contain an explicit `Z`. They are stored as
decimal-string UTC nanoseconds plus the source fractional-digit count, so neither C++ nor
JavaScript claims precision absent from the source.

Canonical total order is `(event UTC ns, provider sequence or absent-last, source
ordinal)`. Source ordinal is the original line number. This is deterministic but is not a
claim about causal exchange order when a provider feed offers no sequence guarantee.
Input-order regressions and exact duplicate records are counted. Duplicates are excluded
from canonical playback but remain in immutable raw evidence. Locked and crossed quotes
are retained and classified.

## Artifacts and lifecycle

Each completed session is a new directory containing:

| File | Meaning |
| --- | --- |
| `raw.jsonl` | exact normalizer input retained for verification |
| `historical_events.jsonl` | canonical ordered trades and L1 quotes |
| `historical_samples.jsonl` | state after every canonical observation |
| `historical_metrics.json` | observed counts, VWAP, spread and quote-state durations |
| `quality_report.json` | duplicate, order, gap, locked/crossed and record counts |
| `manifest.json` | schema/version, build provenance, metadata, coverage and SHA-256 digests |

Provider response pages are also retained under
`workspace/historical/raw/<import-id>/`. Native artifacts are written under a sibling
`.partial` directory, all streams and the manifest are closed and checked, and the whole
directory is renamed into place only on success. A completed output directory is never
overwritten. Failed registry rows remain as evidence, while their request identity can be
retried under a new import ID. The SQLite registry contains trusted IDs and job state, not
event rows. API requests never contain filesystem paths.

The first implementation uses inspectable JSON/JSONL and bounded HTTP pages. Parquet is
deferred until measured session sizes justify another canonical dependency.

## Playback, metrics, and comparison

Playback is a fold over canonical records on a historical event clock. Every event emits
the resulting last trade and available L1 state. Seek in the browser selects one indexed
observation; discrete quotes are never interpolated and playback never sleeps.
Verification hashes all artifacts, regenerates every sample, and reports the first sample
sequence that differs.

Schema 1 metrics state units and methodology. Quote-state duration uses last observation
carried forward between canonical events, with no duration invented after the final
event. VWAP weights reported price by reported quantity. Arrival frequencies divide
canonical record counts by coverage. Return volatility is the sample standard deviation
of consecutive reported-trade simple returns in parts per billion and remains unavailable
until at least three trades exist; no regular interval is inferred.

Synthetic comparison only juxtaposes already-computed metrics and labels incompatible
units, scope, duration, sampling, and data-generating processes. Similarity is not
calibration evidence.

## Providers, credentials, and licensing

The local adapter accepts only a named fabricated fixture. It provides fully offline CI.
The Alpaca adapter requires backend environment variables, an explicit IEX or SIP feed,
an explicit UTC interval, and confirmation of entitlement and possible charges. It uses
bounded pages and retries transient failures. Authentication and response bodies are not
copied into errors.

Alpaca terms and feed entitlements govern use and redistribution. Purchased data must
stay out of Git and unrestricted public endpoints. Databento is only a future adapter
boundary; no unverified dataset behavior is implemented.

## Compatibility and limitations

This adds historical schema and normalization version 1 without changing synthetic run,
trace, accounting, metrics, experiment, replay, or Phase 3B AI schemas and goldens.
Historical imports are not exposed as AI execution tools. Real-provider behavior remains
unverified without user-provided credentials and entitlement.
