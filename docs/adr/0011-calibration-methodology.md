# ADR 0011: Calibration methodology against observed sessions

**Date:** 2026-10-20  
**Status:** Proposed for Phase 5B. No calibration code is written until the owner
accepts this record ([ADR 0009](0009-phase-5-scope.md), option B).

## Question

Can a bounded set of synthetic market parameters be chosen so that a small, fixed list of
stylized facts measured on synthetic runs is close to the same facts measured on observed
Alpaca IEX sessions, and does that closeness hold on sessions the fit never saw?

The answer is a statement about those facts, that symbol, that venue, and those dates. It
is not a statement that the simulator is realistic, predicts prices, or that a strategy's
result in it transfers to a real market (section 1.3, section 9.4).

## Boundary

Calibration reads two existing artifact families and writes a third:

```text
verified historical sessions (ADR 0007) ─┐
                                         ├─> native stylized facts ─> score ─> calibration artifact
synthetic runs of candidate configs ─────┘                                    (selected run config)
```

- Observed prints and quotes never enter `MatchingEngine`, agent observations, or
  accounting. A calibrated config is an ordinary run config; nothing observed is replayed
  into the simulation.
- The run config schema, trace format, engine semantics, and every golden digest are
  unchanged. A calibrated config differs from the baseline only in parameter values.
- Python orchestrates the search and job lifecycle. C++ computes every statistic and every
  score, in integer arithmetic, so a calibration result is reproducible from its inputs.

## Data

- **Provider and feed.** Alpaca historical trades and L1 quotes, `iex` feed only, imported
  through the Phase 4A path with the existing entitlement and cost gate. IEX is one venue:
  every observed fact is labelled `OBSERVED IEX DATA`, single venue, and is not a
  consolidated-market fact. SIP is out of scope until its entitlement is verified.
- **Symbol.** One symbol for Phase 5B, chosen by the owner before import. A second symbol
  may be used only as an extra out-of-symbol test, never in fitting.
- **Window.** One hour per trading day, 10:30 to 11:30 America/New_York, converted to an
  explicit UTC interval per date. This matches the synthetic horizon of one hour and avoids
  the open and close. Half days and days with a halt in the window are excluded before any
  statistic is computed, and the exclusion is recorded.
- **Sessions.** Ten consecutive regular trading days, split chronologically and fixed
  before the first candidate is scored: days 1 to 6 **train**, days 7 and 8
  **validation**, days 9 and 10 **test**.
- **Quality gate.** A session is admitted only if its Phase 4A verification passes and its
  quality report shows two-sided L1 quotes for at least 90% of the window. Rejected
  sessions are recorded with the reason and are not replaced by later dates.
- **Licensing.** Raw and normalized observed data stay in the untracked workspace (ADR
  0007). See open question 3 for aggregate statistics.

## Making the two families comparable

A synthetic run has a full book; an observed session has L1 quotes and reported prints.
Facts are therefore computed on one common projection, by one native function:

- **Synthetic projection.** From a run's trace, emit an L1 record whenever the best bid or
  best ask price or size changes, and a trade record for each trade, both stamped with the
  exchange's simulated time. Depth beyond level 1 is discarded.
- **Observed projection.** The canonical Phase 4A events, unchanged.
- **Prices.** Synthetic ticks are cents (`tick_size_minor` 1). Observed prices are
  micro-dollars and are converted to cents only for tick-denominated spread; relative
  spread uses each side's own units divided by its own midpoint.
- **Reference price.** Each candidate's `reference_price_ticks` and fundamental initial
  value are set to the median opening midpoint of the train sessions, so relative and tick
  spreads are comparable. This is not a fitted parameter.

## Stylized facts

All facts are computed over the full window. Every fact states its unit and denominator in
the artifact, and an undefined fact is reported as undefined, never filled in.

| Group | Fact | Definition |
| --- | --- | --- |
| Spread | `spread_ticks_p10/p50/p90` | Time-weighted quantiles of quoted spread in cents over two-sided time, last observation carried forward |
| Spread | `relative_spread_bps_p50` | Time-weighted median of spread / midpoint, in basis points |
| Arrival | `trades_per_minute` | Trade count / window minutes |
| Arrival | `interarrival_cv` | Coefficient of variation of trade inter-arrival times (burstiness; 1 for Poisson) |
| Arrival | `empty_minute_share` | Share of the 60 one-minute buckets with no trade |
| Returns | `trade_return_lag1_autocorr` | Lag-1 autocorrelation of consecutive trade-price changes (bid-ask bounce) |
| Returns | `midpoint_vol_1m_bps` | Sample standard deviation of 1-minute midpoint log returns, midpoint sampled at minute boundaries only when two-sided, in basis points, not annualized |
| Quote state | `two_sided_share` | Share of window time with both sides present |
| Quote state | `l1_updates_per_minute` | L1 change records / window minutes |
| Quote state | `l1_mean_duration_ms` | Mean time between L1 change records |

Locked and crossed time is reported for observed sessions but is not scored: the synthetic
book cannot lock or cross, and IEX quotes are one venue's book.

## Score

- For each fact `f`, the observed target `T_f` is the mean over train sessions and the
  scale `S_f` is the between-session standard deviation over train sessions, floored at 5%
  of `|T_f|` (and at a fixed absolute floor per unit) so a fact that happens to be stable
  across six days does not dominate.
- A candidate's value `V_f` is the mean over its evaluation seeds. Its distance is
  `d_f = (V_f - T_f) / S_f`.
- The score is the root mean square of `d_f` with each of the four groups weighted equally
  and facts weighted equally within a group. The weights are fixed here and not tuned.
- A candidate is **invalid** if any seed fails, produces fewer than 30 trades, or is
  two-sided for less than 50% of the window (section 14.3). Invalid candidates are kept in
  the artifact and excluded from selection; seeds are never dropped to rescue a candidate.

## Parameters

Searched, each over a bounded integer grid declared in the calibration spec:

| Agent group | Parameters |
| --- | --- |
| Noise (all four share values) | mean wakeup interval, `market_order_ppm`, `max_quantity`, `limit_offset_ticks` |
| Makers (all three share values) | wakeup interval, `half_spread_ticks`, `quote_size`, `level_spacing_ticks` |
| Fundamental | `volatility_milliticks` |

Fixed at the baseline: agent counts and kinds, momentum and value parameters, limits,
`buy_probability_ppm` (500000), the inventory maker's risk terms, and every latency. L1
trades and quotes carry no information about participant latency, so latency is **not
identifiable** from this data and is not calibrated. The stretch item's "latency
calibration" stays unaddressed, and the artifact says so.

## Search

1. **Stage 1.** 256 candidates drawn uniformly from the grid with a recorded search seed,
   plus the baseline config as candidate 0. Each is run on 10 seeds.
2. **Stage 2.** The 10 best valid stage-1 candidates are re-run on 50 fresh seeds.
3. **Selection.** Among stage-2 candidates, the one with the lowest score on the
   **validation** sessions is selected. Train is used for targets and scales, validation
   only for selection.
4. **Test.** The selected candidate and the baseline are each run on 100 further fresh
   seeds and scored once against the **test** sessions. Test sessions are not read before
   this step, and the step is not repeated after looking at its result.

Seed ranges for stage 1, stage 2, and test are disjoint and recorded. Runs are
independent and may execute in parallel; results are ordered by candidate and seed before
any aggregation, so parallelism cannot change the artifact.

## Acceptance and claims

Fixed before any data is imported:

- **Fit holds out of sample** if the selected candidate's test score is lower than the
  baseline's test score, and `|d_f| <= 2` on test for at least 9 of the 12 facts.
- Otherwise the result is a **null result**. It is stored and shown with the same
  prominence as a pass, with the facts that failed.
- Either way the report shows train, validation, and test scores side by side, every
  `d_f` with its seed standard deviation, the stage-1 score distribution, and the
  sensitivity of the score to each parameter across the stage-2 candidates.

What the terminal and the AI tools may say about a calibrated config:

- Allowed: "On the listed facts, this config is within the shown distances of IEX
  sessions of `<symbol>` on `<dates>`, 10:30 to 11:30 ET", with a link to the artifact.
- Not allowed: "realistic", "calibrated to the market", any statement about prices,
  volatility, or liquidity outside those facts, any statement about latency or market
  impact, or any claim that a strategy result in the config would hold in a real market.
- A calibrated config is labelled `CALIBRATED (FIT TO OBSERVED IEX FACTS)` or
  `CALIBRATION NULL RESULT` wherever it is shown, never just `CALIBRATED`.

## Artifacts

A new `calibration` family under `workspace/calibrations/<calibration-id>/`, written to a
`.partial` directory and renamed on success like Phase 4A sessions:

| File | Meaning |
| --- | --- |
| `spec.json` | symbol, dates, split, grid, weights, floors, seeds, engine version |
| `observed_facts.json` | per-session facts and the train targets and scales, with session digests |
| `candidates.jsonl` | every candidate's parameters, per-seed facts, validity, and scores |
| `selected_config.json` | the selected candidate as an ordinary run config |
| `report.json` | train, validation, and test results, acceptance outcome, sensitivity |
| `manifest.json` | versions, build provenance, and SHA-256 of every file |

Verification re-hashes the files, recomputes observed facts from the referenced sessions,
and re-runs a sample of candidate seeds to the same facts.

## Implementation outline (after acceptance)

1. Native: synthetic L1 projection, the stylized-fact function over both projections, and
   the score, with unit tests on hand-built streams and a cross-check that the observed
   projection's facts agree with Phase 4A metrics where they overlap.
2. Native CLI `calibrate-score` and a binding call that score one candidate's runs.
3. Python: calibration spec model, search orchestration as a durable background job,
   artifact writer, verification, and read-only API endpoints.
4. Terminal: a read-only calibration report view and the labels above in the run views.
5. CI exercises the full pipeline on the fabricated fixture sessions, labelled
   `FABRICATED`; no CI step contacts Alpaca.
6. The owner runs the real calibration with their credentials; the stored report is the
   Phase 5B evidence, pass or null.

Calibration is not exposed as an AI execution tool in Phase 5B. AI tools may read and cite
a finished calibration report under the claim rules above.

## Open questions for the owner

1. Which symbol, and which ten trading days (default: the ten most recent complete regular
   sessions before import)?
2. Confirmation that the IEX historical import is within your Alpaca entitlement and cost
   expectations for ten one-hour windows.
3. May aggregate facts and the selected config be committed to Git (like
   `docs/experiments/`), or must the whole calibration artifact stay in the untracked
   workspace under Alpaca's terms?
4. Are the acceptance thresholds above (beat baseline on test, `|d_f| <= 2` on 9 of 12
   facts) the right bar?
