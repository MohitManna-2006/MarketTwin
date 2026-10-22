# Phase 5B calibration: AAPL on IEX (null result)

**CALIBRATION NULL RESULT.** This is the ADR 0011 evidence for Phase 5B, produced by
`services/api/scripts/calibrate_aapl.py` against the committed spec
`configs/calibration/aapl_iex.json`. The fit did not hold out of sample, so no closeness
of the synthetic market to observed facts is claimed.

Calibration `cal-7022609b49` used ten observed sessions of AAPL from the IEX feed on
Alpaca's free plan, 10:30 to 11:30 ET, with every session admitted by the quality gate:

| Role | Days |
| --- | --- |
| train | 2026-10-07, 08, 09, 12, 13, 14 |
| validation | 2026-10-15, 16 |
| test | 2026-10-19, 20 |

The search drew 257 stage-1 candidates (the baseline plus 256 grid draws) over 10 seeds,
kept 10 finalists for stage 2 over 50 seeds, and selected candidate 157 on validation.
Scores are in ppm; lower is closer:

| | Train | Validation | Test |
| --- | ---: | ---: | ---: |
| Selected (candidate 157) | 1,690,598 | 2,140,768 | 1,820,831 |
| Baseline config | 48,536,268 | | 48,884,585 |

The selected config beats the baseline on test, but the pre-registered bar also needs
`|d_f| <= 2` on at least 9 of the 12 facts. It reached 8. Test distances `d_f`, in units
of the train scale, over 100 test seeds:

| Fact | Selected | Baseline | Within 2 |
| --- | ---: | ---: | :---: |
| empty_minute_share | 0.000 | 0.000 | yes |
| interarrival_cv | -0.277 | -2.335 | yes |
| l1_median_duration_ms | +0.608 | +168.465 | yes |
| l1_updates_per_minute | -2.659 | -3.347 | **no** |
| midpoint_vol_1m_bps | -3.290 | -7.695 | **no** |
| relative_spread_bps_p50 | -0.805 | -1.648 | yes |
| spread_cents_p10 | -0.504 | -6.512 | yes |
| spread_cents_p50 | -0.833 | -1.670 | yes |
| spread_cents_p90 | -2.053 | -2.147 | **no** |
| trade_return_lag1_autocorr | -1.264 | -9.503 | yes |
| trades_per_minute | +3.045 | +3.958 | **no** |
| two_sided_share | -0.057 | 0.000 | yes |

The four misses describe one shape: the synthetic book updates its top of book less
often, moves its midpoint less, and has a much tighter tail of wide spreads than IEX,
while printing about twice as many trades per minute. The grid in the spec does not
reach a candidate that fixes these together; this result says nothing about whether a
different agent model would.

## Files

| File | Meaning |
| --- | --- |
| [report.json](report.json) | acceptance outcome, train, validation, and test results, stage-1 distribution, sensitivity |
| [observed_facts.json](observed_facts.json) | per-session facts, targets and scales, session digests |
| [spec.json](spec.json) | the spec as run: symbol, dates, split, grid, weights, floors, seeds |
| [selected_config.json](selected_config.json) | candidate 157 as an ordinary run config |
| [manifest.json](manifest.json) | versions, build provenance, SHA-256 of every artifact file |

Per ADR 0011 owner decision 3, only aggregates are committed. `candidates.jsonl` (every
candidate's per-seed facts) and the raw and normalized observed records stay in the
untracked workspace; the manifest still lists the `candidates.jsonl` hash. The other four
hashes match the files here.

The manifest records `git_revision` `945ee7f-dirty`: the run used the Phase 5B
implementation before it was committed as `9a1fec6`.
