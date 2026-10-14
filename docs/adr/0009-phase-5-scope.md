# ADR 0009: Phase 5 scope

**Date:** 2026-10-13  
**Status:** Proposed. Not accepted until the owner chooses an option.

## Context

Phases 1A through 4B are implemented. `architecture.md` section 20 defines no phase
after 4B, only stretch work. This record lists the candidates, what each touches, and
what each must not do, so the next phase starts from an explicit decision.

## Options

### A. The remaining shock families (recommended first)

Section 9.3 specifies six shock families; ADR 0003 implemented three. The missing ones
are order-flow imbalance, latency degradation, and quote-withdrawal stress.

- Touches: native scenario processes, run config (a schema bump with old schemas kept
  valid and their digests unchanged), trace events, replay, the scenario editor, and
  new reference configs, which the AI research tools then list as named scenarios.
- Fits existing contracts: shocks change observations, delays, or order flow; the book
  still determines every trade (section 9.1).
- Exit: each shock is logged on every transition, replays to the same digest, has a
  reference config, and appears in a paired experiment that shows its effect with
  confidence intervals.
- Size: small to medium. No new subsystem.

### B. Calibration methodology against observed sessions

Stretch item: "more realistic latency/agent calibration after a separately approved
methodology". Phases 4A/4B now provide observed sessions, so the inputs exist.

- First deliverable is the methodology itself (a separate ADR): which stylized facts
  (spread distribution, trade arrival rate, return autocorrelation, quote-state
  durations), which observed sessions, how a fit is scored, how out-of-sample validation
  works, and what the terminal is allowed to claim.
- Must keep: synthetic and observed data in separate artifact families; section 1.3's
  "no promise of realistic calibration without independent validation".
- Size: medium to large; the methodology needs the owner's approval before code.

### C. Multiple synthetic symbols

Stretch item. Several symbols in one run with explicit cross-symbol event ordering.

- Touches: kernel ordering, trace and replay schemas, accounting (per-symbol positions),
  agents, metrics, API, and every terminal view that assumes one book.
- Size: large. It changes the most contracts of any option.

### D. Order-book performance

Stretch item. Optimized book variants and allocation profiling against the stored
benchmark baselines.

- Touches: `core/exchange` internals only; every existing golden digest must stay equal.
- Size: small to medium. No user-visible feature.

### Not proposed

Paper execution or hypothetical fills on observed data, order entry, and brokerage
access stay out of scope (ADR 0007, ADR 0008, section 1.3).

## Recommendation

Phase 5A = option A: it completes the documented scenario model inside existing
contracts. Phase 5B = the option B methodology ADR, because 4A and 4B were built to make
that comparison possible, with implementation only after it is accepted.
