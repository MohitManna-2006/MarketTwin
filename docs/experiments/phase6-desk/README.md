# Phase 6 evidence: the AI desk, its decision ledger, and a fork (fixture model)

**FIXTURE MODEL.** No language model was configured in the development environment, so
these files come from the core's deterministic fixture decision source (`model:
"fixture"`). The fixture widens and leans against inventory past half its position cap
and reduces past four fifths. It is a stand-in that exercises every part of the pipeline.
It is not a model's judgment. To produce the same evidence with a real model, set
`MARKETTWIN_AI_MODEL` and `MARKETTWIN_AI_API_KEY` and run:

```bash
cd services/api
uv run python scripts/desk_evidence.py --source model --export ../../docs/experiments/phase6-desk
# it prints the call count, then:
uv run python scripts/desk_evidence.py --source model --approve-calls 60 \
  --export ../../docs/experiments/phase6-desk
```

The scenario is `configs/latency_shock.json`, seed 42, one hour: makers 1 and 2 are slowed
from minute 20 to minute 30. Agent 3, the inventory-aware maker, is handed to the desk,
which decides every 60 s (60 decisions). The initial directive quotes 3 levels of 100
shares at ±2 ticks with the agent's own 2,000-share cap, and the mandate reads "Make
markets in this name. Keep inventory within 2000 shares, and protect the desk when the
market becomes one-sided or unstable."

Every value below is from the core's own fork reports and experiment statistics, in minor
currency units. Each single-run value is **ONE SAMPLE PATH**.

## The desk run

The desk quoted for 56 decisions and reduced toward flat for 4. Agent 3 ended at
−199,319 total P&L, with a maximum drawdown of 214,597 and 2,385 fills. Every decision is a
`Decision` record in the trace with what the agent had been delivered. Replay verification
answers each one from those records, so it never asks a model.

## Decision ledger

For each decision that changed the directive, the run was forked at that moment with the
directive it replaced, every later decision kept, and the fork stopped at the next
decision. The value is the desk's equity in the run minus the fork's at that point:

| | Count | Notes |
| --- | ---: | --- |
| Forked and verified | 27 | each with an identical prefix |
| Same directive as before | 32 | value 0 by definition, no fork run |
| At the horizon | 1 | no interval left |

The largest values were +16,447 (decision 40, `reduce`) and −13,598 (decision 7, `reduce`).
They are not additive, so their sum is not shown as the desk's edge. For the whole run,
the desk against a fork that kept the initial directive from the first decision is
**−68,474**: on this path, changing its mind cost the desk about that much against never
changing it.

## A fork: makers pull their quotes at minute 25

`fork_report.json` forks the desk run at 25:00, adds a `QuoteWithdrawalStress` on makers 1
and 2, and lets the desk decide again live. The fork verified: 47,276 records (19,445
commands and 27,831 execution records) are byte-identical to the parent's up to the fork.
The first divergence is the shock itself at 25:00.000. The first market divergence is 50 ms
later, when the cancels arrive through the makers' degraded order entry (the latency shock
adds 50 ms from minute 20 to minute 30).

After that the two markets agree again. Trade counts are equal (8,634), the midpoint gap
is 0 at every sample, and only agents 1 and 3 differ, by −255 and +255. On this path one
cancel burst did not change the rest of the hour.

## Across 20 seeds: the desk against the baseline maker

`experiment.json` pairs, on seeds 1 to 20, agent 3 as the desk against agent 3 as the
baseline market maker built from the desk's initial directive. Both work under the same
2,000-share agent limit. Desk minus baseline, paired by seed, 95% Student t intervals:

| Metric | Mean difference | 95% interval |
| --- | ---: | ---: |
| Total P&L | +11,324 | −57,050 to +79,698 |
| Max drawdown | +8,504 | −48,535 to +65,542 |
| Maker spread capture | −50,449 | −66,201 to −34,697 |
| Mean abs. position (millishares) | −194,875 | −344,955 to −44,795 |
| Traded quantity (shares) | −21,127 | −30,732 to −11,522 |

The P&L and drawdown intervals cross zero, so they establish no direction. The desk held
less inventory, traded less, and captured less spread than the baseline maker.

A first run of this script gave the desk a 500-share cap while the baseline kept the
agent's 2,000. It showed the desk ahead on P&L with an interval above zero. That edge came
from the tighter cap, not from the desk's decisions. Since then the comparison keeps the
two caps equal.

## What is not claimed

Nothing here is evidence that a model can trade, that the directive logic would work in a
real market, or about any real instrument. The market is synthetic and uncalibrated (the
Phase 5B calibration was a null result), and the decisions came from a fixture, not a
model.

## Files

| File | Contents |
| --- | --- |
| `desk_run.json` | the desk run's config, terminal summary, and desk details |
| `decisions.json` | every decision: time, directive, position, source, model, tokens |
| `decision_ledger.json` | every ledger entry with its fork run ID and value, and the desk against holding |
| `fork_report.json` | the verified report of the minute-25 fork |
| `experiment.json` | the native results document of the 20-seed comparison |

The runs themselves stay in the untracked `data/phase6-evidence` workspace.
`scripts/desk_evidence.py` regenerates all of them.
