# Phase 3B native investigation demonstration

This is a completed Phase 3B workflow with a **deterministic model fixture** and the **real C++ simulation and statistics engine**. No authenticated live model was available in the development environment. The fixture supplied a typed proposal and short nonnumerical interpretation; it did not compute any financial result.

The request compared baseline and inventory-aware market making during the catalogued liquidity withdrawal scenario across four paired seeds. The approved investigation was `inv-8b2aa719aad2`; it submitted experiment `exp-2beafabd99`. All eight native runs completed. [The authoritative experiment document](experiment.json) records the configuration, per-run digests, paired statistics, and results digest. [Paired values](paired_values.json) show the individual seed values. [The structured research report](report.json) contains claims copied from those native summaries and revalidated before publication; [its Markdown rendering](report.md) was generated from the same validated records. [Tool events](tool_events.json) show actual workflow transitions.

Selected candidate-minus-baseline means, stated in the native metric units:

| Metric | Paired mean difference | 95% Student t interval | Evidence |
| --- | ---: | ---: | --- |
| Total P&L | -2,565 minor units | -42,422.805 to 37,292.805 | `experiment:exp-2beafabd99:metric:total_pnl` |
| Max drawdown | -11,628 minor units | -44,315.119 to 21,059.119 | `experiment:exp-2beafabd99:metric:max_drawdown` |
| Mean absolute position | -784,888 millishares | -1,145,715.909 to -424,060.091 | `experiment:exp-2beafabd99:metric:mean_abs_position` |
| Maker spread capture | 5,527.25 minor units | -11,576.882 to 22,631.382 | `experiment:exp-2beafabd99:metric:maker_spread_capture` |

These are descriptive results for a tiny synthetic sample. The P&L, drawdown, and spread-capture intervals cross zero; they do not establish a directional effect. The inventory measure is lower in this sample. The native experiment results digest is `4e0be712ec914c7e8a0923bbc1c34434b6680e103b68fd237ec29f65cffd1152`.

After the experiment, the inventory-maker cell for seed 1 was materialized as `run-59c4d17e14`. Its recorded digests matched the experiment cell, and the native replay verifier returned `Verified`; see [replay evidence](replay.json). The full materialized run remains in the local ignored `data/research-phase3b-demo` workspace; the committed experiment document contains the expected run digests so it can be reproduced from the recorded configuration.

The local API registry recorded 2,665 ms of wall-clock native experiment execution, 484 ms to materialize the selected cell, and 801 ms for replay verification. These are measurements on this development machine, not simulated market time or performance guarantees. The fixture made no network model request, so no live-model latency or cost is reported.
