# ADR 0002: Phase 1B logical time, canonical trace, and exchange replay

Date: 2026-10-10. Status: accepted.

## Scope

Implement architecture sections 7 and 11 for scripted single-symbol exchange
commands. Trading policies, stochastic processes, latency distributions, accounting,
Parquet export, Python bindings, and services remain in their later phases.

## Ordering and lifecycle

- A single-threaded heap orders commands by `(timestamp_ns, scheduler_sequence)`.
  Scheduler sequences are assigned on insertion, start at one, and never wrap.
  Insertions at the current clock are permitted and follow already queued commands
  at that time. Scheduling into the past and overflowing time/sequence fail explicitly.
- The kernel stamps order arrivals; configs cannot supply authoritative arrival
  metadata. A separate, centrally allocated trace sequence orders commands and
  their synchronous outcomes in dispatch order. The command's trace sequence is
  the order's arrival sequence and each output's causation ID. Scheduler sequence
  is an event identity/tie breaker, not a dispatch-order sequence.
- Events exactly at the horizon execute. An event beyond the horizon stops the
  clock at the horizon without dispatch. An explicit stop executes in queue order;
  later same-time commands do not execute. Queue exhaustion takes precedence over
  an event cap reached on the last available command. Otherwise, the cap stops
  before the next command. These terminal reasons are recorded, not inferred.
- The supported payload variant is submit, cancel, snapshot, or stop. The scheduler
  supports insertion during native dispatch; no Python/per-event callback is added.
- Each `Simulation::run()` starts fresh, including counters and exchange state.
  An empty script is a valid zero-event run. Queue/event/output-byte limits are
  explicit. Exceptions fail the run and never publish a completed manifest.

## Format version 1

- JSON parsing/serialization uses SHA256-pinned nlohmann/json 3.11.3. Configs reject
  unknown/missing fields, duplicate keys, floats where integers are required,
  invalid enums, out-of-range numbers, and nesting deeper than 32. No narrowing
  conversion is used to validate a numeric input. Native config validation also
  applies to direct C++ callers.
- Canonical JSON is UTF-8, compact, with lexicographically sorted ASCII object keys,
  explicit nulls, decimal integer tokens, stable named enums, and no floats.
  Each JSONL record ends in exactly one LF. JSON values are serialized by field;
  C++ padding, memory addresses, native endianness, and hash-container order never
  enter the format. This is a project-specific format, not a claim of RFC 8785.
- `config.json` is the complete canonical scripted input. Its SHA-256 includes
  initialization, limits, seed, units, and the original scheduling order. Agent
  registration is canonicalized by ID. The seed is metadata until stochastic
  scenario processes exist; Phase 1B makes no random draws.
- `commands.jsonl` and `trace.jsonl` have versioned headers linking the run ID,
  engine version, config digest, and unit definitions. Subsequent envelopes include
  schema version, run ID, global trace sequence, logical timestamp, event kind,
  actor (null for system), scheduler sequence, causation ID (null for commands),
  correlation/order ID (null when absent), payload, and SHA-256 payload digest.
- Command records precede mutation. Trade records follow matching order, followed
  by maker status updates and the final incoming status, as defined in ADR 0001.
  Snapshots are explicit commands/checkpoints. A terminal execution record includes
  stop reason, counts, clock, final book digest, and trade digest.
- SHA-256 is implemented over explicit bytes with standard test vectors. File
  digests cover exact bytes including LFs; trade digest covers canonical trade
  payloads plus LFs in execution order. The book digest covers every live order,
  including owner, remaining quantity, and FIFO priority, bids first then asks.
  It is not a digest of only aggregated or depth-truncated quotes.
- `manifest.json` records format/hash IDs, engine/build provenance, config/file
  digests, and the terminal summary. Provenance and output paths are outside the
  deterministic trace digests. No timestamps from the wall clock enter results.
- Artifact publication requires a new output directory. Streams are written to
  `.partial` files, closed and checked, then renamed. The manifest is published
  last via rename; no complete manifest exists on failure. Existing directories
  are refused. Files are never appended to or overwritten by another run.

## Replay and diagnostics

Replay loads the canonical config and deterministically re-creates its schedule.
Every dispatched input must byte-match the command log *before* it reaches a fresh
exchange. Result records must byte-match the independent output log. Outputs are
never submitted back to the exchange. This checks exchange replay and also detects
scheduler regressions and missing/reordered commands; it is not yet agent replay.
Replay verifies EOF, final book/trade digests, terminal summary, and all artifact
checksums. No output artifacts are modified during verification.

A first mismatch reports the trace sequence, expected/actual record, preceding
verified output, and the causal command. Record context includes the relevant
trade/order/snapshot, and the run manifest identifies the source configuration and
build. Missing manifests, truncated/nonterminated lines, schema/version changes,
extra records, invalid configs, and checksum mismatches are explicit failures.
Hashes provide integrity/reproducibility, not authenticity against an attacker
rewriting the entire run. Only supported schema and engine versions are replayed.

## Verification

Scheduler ordering/bounds tests, checked counters, SHA-256 vectors, strict config
fixtures, deterministic CLI artifacts, replay corruption/first-divergence tests,
and cross-toolchain golden digests complement the Phase 1A invariant suite.
Exchange benchmarks measure new/crossing/cancel operations with fixture creation
outside timed regions. Stored baselines identify workload, hardware, toolchain,
source revision, repetitions, and variance; CI smoke has no timing threshold.
