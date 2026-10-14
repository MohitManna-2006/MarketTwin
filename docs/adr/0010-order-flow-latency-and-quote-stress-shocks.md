# ADR 0010: Order-flow, latency, and quote-withdrawal shocks

**Date:** 2026-10-14  
**Status:** Accepted for Phase 5A

## Scope

Architecture section 9.3 lists six shock families. ADR 0003 implemented three
(`FundamentalJump`, `LiquidityWithdrawal`, `VolatilityRegime`). This ADR adds the other
three and changes nothing about the first three. As before, a shock changes what agents
see, how fast their messages travel, or how they send orders. The book still sets every
trade price.

## The three shocks

| Kind | Shape | Effect |
| --- | --- | --- |
| `OrderFlowImbalance` | `[start, end)`, noise agents | Replaces each agent's buy probability and market-order share, and scales its arrival rate by `arrival_rate_ppm` (0.01x to 100x) |
| `LatencyDegradation` | `[start, end)`, any agents | Adds `added_order_entry_ns`, `added_report_ns`, and `added_market_data_ns` to the agent's three paths (each 0 to 1e15 ns, at least one nonzero) |
| `QuoteWithdrawalStress` | instant, any agents | Each agent, in list order, asks to cancel every working order it has not already asked to cancel, oldest first |

The design choices:

- **Order flow is changed through the policy's controls, not its parameters.** A noise
  agent draws the same four random numbers per wakeup as before, so its streams stay
  aligned and paired comparisons remain valid. The rate change applies to the next wakeup
  the agent schedules: the wakeup already queued when the shock begins or ends keeps its
  time. Only noise agents can be named. Momentum and value agents trade on signals, and
  forcing their direction would turn them into scripted flow.
- **Latency is added, not multiplied.** A multiplier does nothing to an agent with zero
  configured latency. Each path stays first-in first-out on its existing channel, so when
  the degradation ends, a message sent afterwards waits behind those sent during it. A
  message never overtakes another from the same agent.
- **Quote stress is a burst of cancels, not a withdrawal regime.** The cancels go through
  each agent's own order-entry latency, jitter, and message budget, so they reach the
  exchange interleaved with everything else in flight. A cancel the budget refuses is
  counted in `intents_dropped`. Policies are unchanged: a maker requotes when its own
  logic next runs. For a sustained absence, use `LiquidityWithdrawal`.

## Validation

Each named agent must exist, appear once in the shock, and have an eligible policy.
Intervals of the same kind may not overlap for one agent, as already required for
withdrawals, because any combination rule would be a guess. Shocks of different kinds may
overlap. Instants may repeat. All bounds are checked in the core, and
`services/api/markettwin_api/config_models.py` mirrors the static ones for field-level
errors.

## Compatibility

The run config schema stays at 2 (3 with accounting): the `shocks` array accepts three new
`kind` values. Every existing config parses, canonicalizes, and digests exactly as before,
and every golden summary is unchanged. The trace format is unchanged: transitions are
`ScenarioShock` records that carry the full shock definition, and the cancels are ordinary
`CancelArrival` commands. The engine version stays 0.4.0. A run that uses a new shock is
rejected by an older engine as an unknown shock kind rather than misread.

## Reference configs and evidence

- `configs/order_flow_shock.json`: the four noise traders buy 80% of the time, send 60%
  market orders, and arrive at twice their rate from minute 20 to minute 30.
- `configs/latency_shock.json`: the three makers' orders and cancels are slowed by 50 ms,
  reports by 20 ms, and market data by 50 ms from minute 20 to minute 30.
- `configs/quote_stress_shock.json`: all three makers cancel every working quote at
  minute 30.
- `configs/experiments/latency_shock_makers.json` compares the baseline maker with the
  inventory-aware maker as agent 3 over 100 paired seeds of the latency shock. The stored
  report is in `docs/experiments/phase5a-latency-shock/report.txt`. It is a comparison of
  two strategies inside one stylized market. It is not evidence about real latency.

Integration tests check each shock's mechanism directly: the order mix and rate inside
and outside the window, exact arrival times with FIFO holding after the window ends, the
size and timing of the cancel burst and the requote, rejection of every invalid case, and
byte-exact replay of a run that uses all three.
