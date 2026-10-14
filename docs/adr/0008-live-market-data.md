# ADR 0008: Live market data

**Date:** 2026-10-13  
**Status:** Accepted for Phase 4B

## Boundary

Live data is the observed-market subsystem of [ADR 0007](0007-observed-historical-market-data.md)
running against a stream instead of a file. Provider trades are reported prints and
provider quotes are observations. Neither enters `MatchingEngine`, the simulation clock,
agent observations, or accounting. Phase 4B contains no order entry, brokerage, paper
execution, hypothetical fill, or strategy P&L.

```text
provider WebSocket -> FeedGateway (Python) -> capture file (when recording)
                                           -> bounded inbox -> one native batch call
                                           -> per-client bounded queues -> browser WebSockets
capture file -> finalize -> Phase 4A importer -> ordinary historical session per symbol
```

Python owns the provider connection, authentication, subscription, reconnection, fan-out,
and durable capture. C++ (`markettwin::live::LiveNormalizer`) owns every market number:
decimal prices, UTC timestamps, validation, classification, and per-instrument state. It
is called once per batch of frames, never per message. Prices and timestamps go through
the same checked conversions as Phase 4A (`core/src/historical/observed_values.*`), so a
live record and a historical record of the same print agree exactly.

## Feeds and labelling

| Feed | Endpoint | Label shown everywhere | Data |
| --- | --- | --- | --- |
| `iex` | Alpaca stock stream, IEX | `OBSERVED IEX DATA` | one venue, real |
| `test` | Alpaca test stream (`FAKEPACA`) | `TEST DATA` | provider test data |
| `mock` | `live_mock` on the loopback only | `FABRICATED MOCK DATA` | fabricated |

The gateway never switches feed by itself. SIP is not offered: it needs a paid
entitlement this phase has not verified. The mock feed accepts only a `ws://` loopback URL
and is never sent real credentials. Recordings carry the feed's label into the historical
library (`RECORDED LIVE DATA`, `ALPACA TEST STREAM DATA`, or `FABRICATED TEST DATA`).

## Gateway state machine

`STOPPED -> CONNECTING -> AUTHENTICATING -> SUBSCRIBING -> CONNECTED`, with `DEGRADED`
when the subscription acknowledgment differs from the watchlist or the provider reports
an error, `RECONNECTING` with capped exponential backoff and jitter after a transport
loss or retryable error, and `STOPPED` with the error kept after an unrecoverable
refusal (bad credentials, entitlement, connection limit). `CONNECTED` describes the
transport and subscription only; market activity is derived separately from observation
times, so a healthy connection on a weekend is reported as connected and quiet.

The provider allows one connection per endpoint on most plans. A per-user lock file
prevents a second MarketTwin process from taking it, `MARKETTWIN_LIVE_AUTOSTART` is off by
default, and the symbol ceiling (`MARKETTWIN_LIVE_MAX_SYMBOLS`, 30 for the free plan) is
enforced before the provider enforces it.

## Clocks, ordering, and classification

Three clocks stay separate in every record: the source event timestamp, the gateway
receipt time (wall and monotonic), and native processing time. Ages are differences of
monotonic values. The single cross-domain figure, `source_age_ns`, is labelled as
including unknown clock offset and transit time.

The provider supplies no sequence number, so arrival order is not treated as exchange
order. Per instrument and kind, a record older than the newest one seen is classified
`out_of_order` and does not replace state; an exact repeat inside a bounded window is
`duplicate`. Both are still emitted, flagged, and counted. A trade never changes quote
state and a quote never changes trade state. An absent quote side is null, never zero.
Corrections and cancels are counted and kept in recordings but not applied.

Freshness is per instrument (`stale_after`), and the market-session hint uses the New
York wall clock only. It never claims the market is open: holidays and early closes are
not known.

## Bounded fan-out and the browser protocol

Three resources are bounded and each overflow is counted and announced: the inbox between
gateway and core (frames are dropped and every client is told), each client's queue (that
client gets a `gap` and a fresh snapshot), and the capture file (recording stops and says
why). Nothing grows without limit and no loss is silent.

The browser stream (`/api/v1/live/stream`, protocol version 1) sends `hello`, `snapshot`,
`events`, `status`, `gap`, and `pong` messages with a hub-local `seq`. A client that sees
`seq` skip sends `resync` and receives a snapshot. `seq` is not a provider or exchange
sequence.

## Recording

A capture is one append-only JSON-lines file: a header, every provider data frame
verbatim with its receipt times, connection lifecycle events in line with the frames they
separate, and a footer that is absent after a crash. Nothing in it is rewritten.
Recording stops at the configured byte, message, or duration ceiling. Finalizing reads
the capture back, lets the native core decide which records are valid, and imports one
ordinary Phase 4A session per instrument, which is normalized, ordered, digested, and
verified like any other import. Finalizing is idempotent, and a capture left open by a
previous process is finalized from what reached the disk at the next start.

A recording is never presented as a complete record of the market: the feed is one venue
or a test stream, and every upstream interruption is listed as a coverage gap.

## Access control

Reading live state is open to whoever can reach the API, like the rest of this local
service. Starting or stopping the feed, editing the subscription, and recording act on
the single provider session and on local storage, so they are accepted only from the
loopback interface or with `MARKETTWIN_LIVE_CONTROL_TOKEN` (compared in constant time).
Browser requests from a foreign origin are refused on both HTTP and the WebSocket, because
a WebSocket is not protected by CORS. Credentials stay in backend environment variables
and are never sent to the browser or copied into errors.

## Compatibility and limitations

This adds live schema 1 and the `live_recordings` registry table. It does not change
synthetic run, trace, accounting, metrics, experiment, replay, Phase 3B AI, or historical
schema 1 artifacts; the historical session view gains two `observation_kind` labels. Live
data is not exposed to AI research tools.

Verified in CI and locally against the mock stream only: unit tests for the normalizer,
gateway state machine tests against the mock, API tests, and a browser test that connects,
records, and files the recording as a historical session. Behaviour against Alpaca's real
IEX and test endpoints is unverified without user-provided credentials. Alpaca terms
govern use and redistribution of the IEX feed; recordings stay out of Git.
