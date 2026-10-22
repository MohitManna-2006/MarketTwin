# ADR 0012: Timeline forks

**Date:** 2026-10-21  
**Status:** Proposed for Phase 6. Implementation starts after the owner accepts it and
answers the [open decisions](#open-decisions).

## Question

Given a finished run, what would have happened if one thing had been different from a
chosen moment on: a shock added, a pending shock removed, or one participant switching
strategy? Answering that needs two runs whose histories are provably identical up to that
moment and differ only in the change.

A plain second config cannot answer it. Adding a shock to a config schedules its
transitions when the queue is seeded, so every later scheduler sequence moves and the two
traces differ from the first record, even where behavior is the same. Nothing then shows
where the futures actually part. A fork makes the shared past a checked fact rather than
an assumption.

## What a fork is

A fork is an ordinary run config with one more block, `forks`: an ordered list of
**stages**. Each stage names its parent and the change it makes at one instant.

```json
"forks": [
  {
    "parent_run_id": "latency-shock",
    "parent_config_digest": "9f2c…",
    "at_ns": 2520000000000,
    "add_shocks": [ { "kind": "QuoteWithdrawalStress", "at_ns": 2520000000000, "agents": [1, 2, 3] } ],
    "remove_shocks": [1],
    "policy_overrides": [ { "agent_id": 3, "policy": { "kind": "InventoryMarketMaker", "…": "…" } } ]
  }
]
```

- Every field outside `forks` and `run_id` is the parent's, unchanged. Removing the last
  stage and restoring `parent_run_id` must give a config whose canonical digest is
  `parent_config_digest`; validation checks this, so a fork cannot quietly change a seed
  or a parameter.
- A fork of a fork appends a stage. Stages are in non-decreasing `at_ns`, so a run's
  `forks` list is its path from the root of a tree of timelines.
- `at_ns` must be before the parent's horizon. A run that stops before reaching a stage
  fails with `ForkNotReached`.

## The barrier

The kernel holds each stage until the moment it applies. Before dispatching the first
event whose timestamp is at or after `at_ns`, it applies the stage, in this order:

1. Writes one `ForkPoint` execution record, timestamped `at_ns`, carrying the stage.
2. Drops every queued transition of each removed shock. A removed shock must not have
   started: all its transitions are at or after `at_ns`. Ending a shock early is not a
   stage change; add a shorter shock instead.
3. Schedules the transitions of each added shock, indexed after the parent's shocks. All
   their times are at or after `at_ns`. Their scheduler sequences come after every event
   already queued, so an event the parent had queued for exactly `at_ns` still runs first.
4. Replaces each overridden agent's policy. The agent keeps its ID, random stream, wakeup
   schedule, latency, limits, position, working orders, and any shock controls in force,
   because the environment owns those (ADR 0003). Only the decision logic changes, and the
   new policy starts with no memory: an inventory maker's volatility estimate or a
   momentum agent's lookback is empty at `at_ns`.

Nothing before the barrier depends on the stage, and the barrier itself draws no random
numbers and schedules nothing when a stage is empty. Hence:

- **Prefix identity.** Every trace record before `ForkPoint` is byte-identical to the
  parent's record at the same position, except `run_id`.
- **The empty fork.** A stage with no changes ends with the parent's book digest, trade
  digest, and every per-agent accounting and metric value. The trace, accounting, and
  metrics digests still differ, because those files carry the run ID and config digest
  and the recorder sequences after `ForkPoint` move by one. This is the main correctness
  test.

Validation runs on the combined config: the parent's shocks minus the removed ones plus the
added ones must satisfy every ADR 0003 and ADR 0010 rule, judged against each agent's
policy after the fork. Forking a maker into a momentum trader while a later withdrawal
still names it is rejected.

## Verifying a fork

`markettwin fork-verify --parent DIR --fork DIR` checks, in order, and stops at the first
failure with a typed code:

1. Both directories pass ordinary replay verification.
2. The fork's last stage relates its config to the parent's config digest as above.
3. Both commands and execution channels match byte for byte, with `run_id` normalized, up
   to the fork's `ForkPoint`. It reports the record counts and one **prefix digest** over
   the normalized prefix, which is the same value for both runs.
4. It finds the **first divergence**: the first record after the prefix whose kind,
   timestamp, actor, correlation, or payload digest differs from the parent's (recorder
   and causation sequences are ignored, since `ForkPoint` shifts them by one). It also
   reports the **first market divergence**, the first differing order, cancel, or trade,
   because the first differing record is often the added shock itself. Either may be
   "none": a change can have no effect before the horizon.

It writes `fork_report.json` into the fork's directory:

| Section | Contents |
| --- | --- |
| `prefix` | `at_ns`, record counts per channel, prefix digest |
| `divergence` | first divergence and first market divergence, each with both records |
| `agents` | per agent, parent and fork values and the difference of total, realized and unrealized P&L, max drawdown, final and mean absolute position, fills |
| `market` | trades, volume, and the spread and midpoint at each sample after `at_ns` in both runs, and their absolute difference over time |
| `digests` | both runs' config, trace, book, trade, accounting, and metrics digests |

All values are integers in the existing units, computed in C++.

## What a fork may claim

A fork is one sample path. Its difference is what the change did on that path from that
moment, given that exact history. It is not an expected effect, and the terminal shows
every fork comparison labelled `ONE SAMPLE PATH`.

For an effect across paths, a stage can be a variant of an experiment (schema 2): the
variant carries one stage, the baseline carries none, and every seed's pair then shares
its history up to `at_ns` by construction. The existing paired statistics and intervals
apply unchanged (ADR 0004). Experiment schema 1 files parse and digest as before.

Synthetic and observed data stay separate. A fork's parent is always a synthetic run.
Forking an observed session is out of scope, since observed prints never enter the
book (ADR 0007).

## Compatibility

- A config with `forks` serializes as run config schema 4. Configs without it parse,
  canonicalize, and digest exactly as before, and every golden summary is unchanged.
- Trace format 1 gains one execution record kind, `ForkPoint`. Runs without forks never
  write it. An older engine rejects schema 4 rather than misreading it.
- Engine version 0.5.0.
- No snapshotting: a fork re-executes its prefix. That costs one full run, about as long
  as the parent, and the re-execution is what proves the prefix.

## Implementation

1. Native: the `forks` block in `RunConfig` and its validation; `Scheduler::peek_time()`;
   the barrier in the kernel; `Population::replace_policy`; `ForkPoint` in the recorder
   and codec; `verify_fork` and `fork_report.json`; `markettwin fork-verify`. Unit and
   integration tests: the empty fork, each change on its own, fork of a fork, every
   rejection, replay of a fork, and `fork-verify` catching an altered prefix record.
2. Experiments: an optional `fork` stage per variant, experiment schema 2, a reference
   experiment, and a committed 100-seed report in `docs/experiments/phase6-fork/`.
3. Binding and API: `POST /api/v1/runs/{id}/forks` (stage fields and a title) queues the
   fork as an ordinary run; the registry records `parent_run_id` and `fork_at_ns`;
   `GET /api/v1/runs/{id}/timeline` returns the tree; `GET /api/v1/runs/{id}/divergence`
   returns the verified `fork_report.json`. Forks use the existing queue and limits.
4. Terminal: on a run's charts, **Fork here** at the cursor opens an editor with the
   scenario editor's shock forms, the parent's pending shocks to remove, and a policy
   picker per agent. A **Timeline** view draws the tree. The comparison overlays both runs
   with the shared prefix shaded and badged `PREFIX VERIFIED · n records identical`, the
   divergence-over-time curve, the first-divergence card linking to the trace records, the
   agent table, and **Repeat over seeds**, which creates the experiment of step 2.
5. AI tools may read and cite a verified fork report under the claim rules above. Creating
   forks is not an AI tool in Phase 6.

## Open decisions

1. **Policy switches in scope?** Recommended yes: "what if agent 3 had switched to the
   inventory maker at 10:42" is the most useful question, and the environment already
   owns everything that must survive the switch. The cost is the cold-start caveat.
2. **Fork experiments (step 2) in Phase 6, or a later phase?** Recommended in Phase 6,
   because without them every fork result is one path.
3. **Reference evidence.** Recommended: fork `configs/latency_shock.json` (seed 42) at
   minute 20, switching agent 3 to the inventory maker, as the committed single fork; and
   the same stage over 100 seeds as the committed experiment.
