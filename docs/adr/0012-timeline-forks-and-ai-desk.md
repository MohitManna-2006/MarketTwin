# ADR 0012: Timeline forks and the AI desk

**Date:** 2026-10-21  
**Status:** Accepted for Phase 6 on 2026-10-22 and implemented. The owner's decisions and
what changed in implementation are in [Decisions on acceptance](#decisions-on-acceptance).

## Question

Two questions, one mechanism:

1. Given a finished run, what would have happened if one thing had been different from a
   chosen moment on: a shock added, a pending shock removed, or one participant changing
   strategy?
2. When a language model trades as one participant, which of its decisions helped, and by
   how much, against the same history?

Both need two runs whose histories are provably identical up to a moment and differ only
in one change. A plain second config cannot give that. Adding a shock schedules its
transitions when the queue is seeded, so every later scheduler sequence moves and the two
traces differ from the first record, even where behavior is the same. Nothing then shows
where the futures part. A **fork** makes the shared past a checked fact. The **AI desk**
records each model decision as the same kind of change, so every decision can be forked
against the alternative of not making it.

## Part 1: Timeline forks

### What a fork is

A fork is an ordinary run config with one more block, `forks`: an ordered list of
**stages**. Each stage names its parent and the change it makes at one instant.

```json
"forks": [
  {
    "parent_run_id": "latency-shock",
    "parent_config_digest": "9f2c…",
    "at_ns": 1200000000000,
    "add_shocks": [ { "kind": "QuoteWithdrawalStress", "at_ns": 1200000000000, "agents": [1, 2] } ],
    "remove_shocks": [1],
    "policy_overrides": [ { "agent_id": 3, "policy": { "kind": "InventoryMarketMaker", "…": "…" } } ],
    "decisions": "hold",
    "horizon_ns": null
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
- `horizon_ns` may only shorten the run, to a time after `at_ns`. It exists so decision
  forks (Part 2) stop where they are measured.
- `decisions` applies only to AI desk agents and is described in Part 2.

### The barrier

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
- **The empty fork.** A stage with no changes has no divergence at all under the
  comparison below, and every per-agent accounting and metric value equals the parent's.
  Its digests still differ: `ForkPoint` takes one recorder sequence, so every later
  record's sequence, and the arrival sequences inside trades and resting orders, move by
  one, and the files carry the run ID and config digest. This is the main correctness
  test.

Validation runs on the combined config: the parent's shocks minus the removed ones plus the
added ones must satisfy every ADR 0003 and ADR 0010 rule, judged against each agent's
policy after the fork. Forking a maker into a momentum trader while a later withdrawal
still names it is rejected.

### Verifying a fork

`markettwin fork-verify --parent DIR --fork DIR` checks, in order, and stops at the first
failure with a typed code:

1. Both directories pass ordinary replay verification.
2. The fork's last stage relates its config to the parent's config digest as above.
3. Both commands and execution channels match byte for byte, with `run_id` normalized, up
   to the fork's `ForkPoint`. It reports the record counts and one **prefix digest** over
   the normalized prefix, which is the same value for both runs.
4. It finds the **first divergence**: the first record after the prefix whose kind,
   timestamp, actor, correlation, or payload differs from the parent's. Recorder,
   causation, and scheduler sequences are ignored, and so is a trade's arrival sequence,
   since `ForkPoint` and added shocks shift them. It also
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

All values are integers in the existing units, computed in C++. No snapshotting: a fork
re-executes its prefix, which costs one run and is what proves the prefix.

## Part 2: The AI desk

### A model as one participant

A new policy kind, `ModelDirected`, makes one scenario agent a desk whose strategy a
language model sets. The model never sends orders. It chooses a **directive**; a native
execution layer carries it out on every wakeup, deterministically, through the agent's
own limits, latency, and message budget, and the book decides every fill (section 16.2:
no reliance on the model for matching or accounting).

```json
{
  "mode": "quote",
  "half_spread_ticks": 3,
  "skew_ticks": -1,
  "quote_size": 50,
  "levels": 2,
  "max_position": 500,
  "rationale": "inventory long after one-sided fills; widen and lean offers"
}
```

| Mode | The execution layer |
| --- | --- |
| `quote` | quotes both sides like the baseline maker, shifted by `skew_ticks`, with the given spread, size, and levels |
| `pause` | cancels its working orders and sends nothing |
| `reduce` | cancels quotes and sends limit orders at the touch toward zero position, at most `quote_size` per wakeup |

Every number is bounded by the policy's config and the agent's `AgentLimits`; an
out-of-bounds directive is rejected and the previous one stays in force. `rationale` is
at most 280 characters, stored and shown, and never treated as a claim.

### Decision points

The policy config sets `decision_interval_ns` (for example 60 s) and an initial
directive. At each multiple of the interval the kernel hits a **decision barrier**,
which works like a fork barrier: before dispatching the first event at or after that time
it asks a `DecisionSource` for the next directive, writes a `Decision` execution record
(time, agent, observation digest, directive, model ID, token usage), and applies it.

What the model sees is only what that agent has been delivered, built natively from its
`AgentObservation` and the agent's own reports: the delivered top of book over the last
interval, its own fills, position, working orders, controls in force, the time remaining,
and the owner's **mandate** text from the config (for example "make markets in this
name, keep inventory within 500 shares"). It never sees the fundamental, the book beyond
delivered data, other agents, or the shock schedule.

### Replay without the model

Model output is not reproducible, so the recorded decision is the source of truth:

- A **live** run's `DecisionSource` calls the model through the existing provider adapter
  (ADR 0006) from the API process. The native call blocks at the barrier; the binding
  releases the GIL while the run executes and takes it for the callback.
- **Replay** verification answers every barrier from the trace's `Decision` records, in
  order, and must reproduce the trace byte for byte. It never calls a model. A missing or
  extra decision fails with `DecisionMismatch`.
- A provider error, timeout, or invalid directive at a barrier keeps the previous
  directive and records the `Decision` with `source: "fallback"` and the error code, so a
  flaky provider never fails or silently alters a run.

### Decisions in forks

A fork of a desk run copies the parent's recorded decisions into the stage. Before
`at_ns` each is written again byte for byte, including its source, model, and token
counts, after checking that the recomputed observation has the recorded digest, so the
prefix replays exactly. A mismatch fails the run with `DecisionMismatch`. Decision
records name their source: `model` and `fallback` for a source's answer, `hold`,
`parent`, and `override` for a stage's. After `at_ns`, `decisions` is one
of:

- `"hold"`: the directive in force at `at_ns` stays to the horizon. Forking the first
  decision with `hold` is the desk against a static version of itself.
- `"parent"`: later barriers take the parent's recorded decisions at the same times,
  although the path they now meet differs.
- `"live"`: the model decides again in the new future, so the desk reacts to the change.
  A shock added to a desk run with `live` asks "how would the AI have handled this?"

A stage may also replace the directive at `at_ns` itself; that is how one decision is
swapped out.

### Decision attribution

For a finished desk run with decisions `d_1 … d_n` at `t_1 … t_n`, the **decision ledger**
forks the run at each `t_k`, replaces `d_k` with `d_{k-1}` (the directive it superseded),
takes `"parent"` for later decisions, and sets `horizon_ns = t_{k+1}`. Each fork's
**decision value** is the desk's mark-to-market P&L at `t_{k+1}` in the parent minus the
fork: what making that change, rather than not, was worth over its own interval on this
path. Every value comes with its verified prefix, and is read from the fork report's
accounting at the end of the window both runs share. A decision at the horizon has no
interval and is listed as `at_horizon`.

The values are not additive. A decision's effect continues past its interval and
interacts with later ones, so the ledger never shows a sum as the desk's total edge. The
total is measured separately by the `hold` fork from `t_1`. With `n` decisions the ledger
costs `n` short runs and no model calls; they run on the existing worker pool.

### Across seeds

A desk experiment pairs the desk with the baseline maker built from its initial directive,
as the same agent, over seeds. The API runs each desk cell live once, several at a time
because each mostly waits on the model, and the native experiment then reruns every cell
from those recorded decisions, so the existing paired statistics and intervals apply
(ADR 0004) and the model is asked once per decision. The desk's initial cap should equal
the agent's limit, which is all the baseline maker has; otherwise the comparison measures
the cap. This is the only claim
about the desk beyond one path, and its cost is `seeds × decisions` model calls, shown and
approved before it starts, as with historical imports. Fork experiments without a desk use
the same schema: a variant carries one stage, the baseline none, and every seed's pair
shares its history up to `at_ns` by construction.

## What may be claimed

- A fork or a decision value is one sample path, labelled `ONE SAMPLE PATH` wherever it
  is shown. It is what the change did from that moment given that history, not an
  expected effect.
- A desk result is a statement about one model, one mandate, and one synthetic market.
  It is not evidence that the model can trade, that the directive logic would work in a
  real market, or about any real instrument (section 1.3). The desk is never connected to
  observed or live data and has no broker path (section 16.2, ADR 0008).
- Model IDs, prompt and mandate digests, and token cost are recorded with every desk run.
  Rationales are shown as the model's words, not as explanations of the outcome.

## Compatibility

- A config with `forks` or a `ModelDirected` agent serializes as run config schema 4.
  Every older config parses, canonicalizes, and digests exactly as before, and every
  golden summary is unchanged. An older engine rejects schema 4 rather than misreading it.
- Trace format 1 gains two execution record kinds, `ForkPoint` and `Decision`. Runs
  without forks or desks never write them.
- Experiment schema 2 adds an optional stage per variant; schema 1 files are unchanged.
- The engine version stays 0.4.0. Replay checks a run's manifest against the engine
  version, so a new version would make every stored run unverifiable; an older engine
  already rejects schema 4.
- CI uses a deterministic fixture `DecisionSource` (a rule that widens when inventory
  grows), labelled `FIXTURE MODEL`, and never calls a provider, as in Phase 3B.

## Implementation

1. Native forks: the `forks` block and its validation, `Scheduler::peek_time()`, the
   barrier, `Population::replace_policy`, `ForkPoint`, `verify_fork`, `fork_report.json`,
   and `markettwin fork-verify`. Tests: the empty fork, each change alone, fork of a fork,
   a shortened horizon, every rejection, replay of a fork, and `fork-verify` catching an
   altered prefix record.
2. Native desk: `ModelDirected` with its execution layer, the decision barrier, the
   observation builder, `Decision` records, the `DecisionSource` interface with recorded
   and fixture sources, and replay from recorded decisions. Tests: each mode's orders on
   hand-built observations, limits and rejection, replay byte equality, `DecisionMismatch`,
   fallback on a failed source, and that the observation contains nothing beyond the
   agent's deliveries.
3. Experiments: schema 2 stages and desk variants; reference experiments.
4. Binding and API: run with a Python `DecisionSource`; `POST /api/v1/runs/{id}/forks`;
   `GET /api/v1/runs/{id}/timeline` (the tree); `GET /api/v1/runs/{id}/divergence`;
   `POST /api/v1/desks` (scenario, agent, mandate, cadence, model) with a cost estimate
   and an approval step; `GET /api/v1/runs/{id}/decisions` and
   `POST /api/v1/runs/{id}/decision-ledger`. Everything runs on the existing queue and
   limits, and the registry records parents, fork times, and model usage.
5. Terminal: **Fork here** on a run's charts, with the scenario editor's shock forms, the
   parent's pending shocks to remove, and a policy picker; a **Timeline** tree; a fork
   comparison with the shared prefix shaded and badged
   `PREFIX VERIFIED · n records identical`, the divergence curve, the first-divergence
   card, and the agent table. An **AI Desk** page: the decision timeline with directive
   chips and rationales on the price chart, the decision ledger as bars, each opening its
   fork, the desk-versus-hold fork, and **Repeat over seeds**.
6. AI researcher tools (ADR 0006) may read and cite verified fork reports, decision
   ledgers, and desk experiments under the claim rules. Creating forks is not an AI tool
   in Phase 6.
7. Evidence in `docs/experiments/phase6-desk/`: one desk run on
   `configs/latency_shock.json` with its decision ledger, and the desk against the
   baseline maker over seeds.

## Decisions on acceptance

Recorded on 2026-10-22, when the owner asked for the build to go ahead:

1. **Model and budget.** No model was configured, so the committed evidence uses the
   `FIXTURE MODEL` and says so. `services/api/scripts/desk_evidence.py --source model`
   produces the same files with the configured model once its exact call count is
   approved. Budgets: 600 decisions per desk run and 50 seeds per desk experiment by
   default (`MARKETTWIN_DESK_MAX_DECISIONS`, `MARKETTWIN_DESK_MAX_SEEDS`).
2. **Directive vocabulary:** `quote`, `pause`, `reduce`, as proposed.
3. **Policy switches in plain forks:** yes, with the cold-start caveat.
4. **Fork experiments:** in Phase 6, as experiment schema 2. A fork variant of an AI desk
   is rejected, because only a run records a desk's decisions; the API instead repeats a
   desk run as the desk-against-baseline experiment above.
5. **Reference evidence:** in `docs/experiments/phase6-desk/`, with the desk's cap at the
   agent's 2,000-share limit instead of the proposed 500, so that the desk and the
   baseline maker are compared under the same cap. The README records the first run's
   500-share comparison and why it was replaced.
