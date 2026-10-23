# ADR 0013: Shock impact research in the AI workflow

**Date:** 2026-10-22
**Status:** Accepted for Phase 3B

## Context

[ADR 0006](0006-supervised-ai-research.md) limited new AI plans to a baseline maker versus
inventory-aware maker comparison and rejected "what does this shock do?" because the
native experiment schema could not express a shocked-versus-unshocked pair. Phase 6
([ADR 0012](0012-timeline-forks-and-ai-desk.md)) added experiment schema 2, in which a
variant forks another variant's cell of the same seed at one instant. That expresses the
pair exactly, so the restriction no longer has a technical reason.

## Decision

Add one executable intent, `measure_shock`. The model proposes only a scenario, a seed
count, metrics, and optionally a subject agent; the backend builds the experiment:

- The scenario must be a catalogued scenario with exactly one shock. The base config is
  the scenario unchanged, so the `shock` variant is the scenario as published.
- The `no_shock` variant forks `shock` at the shock's start time and removes it
  (`remove_shocks: [0]`). The comparison is `shock` minus `no_shock`, so a positive
  difference is what the shock added.
- Both cells of a seed are byte-identical up to the shock's start by construction, and
  removing a shock at the instant it would start is accepted by native validation only
  while it has not started. Every paired difference is therefore the shock's effect from
  its start to the horizon on that seed's history, not a difference between independent
  markets.
- The subject agent is the one the model names if the scenario has it; otherwise the
  first agent the shock targets, or agent 1 for a market-wide shock. Market metrics do
  not depend on the subject.
- The plan carries the shock's kind and window so the reviewer sees what will be removed
  before approving. Approval, digest binding, native statistics, claim verification, and
  the no-digits rule for model prose are unchanged.

The model cannot add, edit, or time a shock, pick a fork instant, or combine a shock
question with a strategy change. Those requests stay `UNSUPPORTED`.

## Consequences

- No C++, schema, or digest change: the plan is an ordinary schema 2 experiment that the
  existing Experiment Lab and replay verification already handle.
- A scenario with several shocks is rejected rather than measured as a bundle; none is
  catalogued today.
- The effect is measured on the scenario's own agent mix and parameters. It is a
  synthetic, uncalibrated result (Phase 5B was a null result) and the report says so.
- `ResearchPlan.baseline` and `candidate` widen from fixed literals to the two variant
  pairs the backend can build.
