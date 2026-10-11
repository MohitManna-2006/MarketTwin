# ADR 0003: Phase 2A agents, random streams, latency, and scenario processes

Date: 2026-10-10. Status: accepted.

## Scope

Implement architecture sections 7.2, 7.3, 8, and 9 inside the native run: five trading
policies, the delivered-observation boundary, per-agent latency, a deterministic random
number generator, a synthetic fundamental value, and three shock kinds. Authoritative
cash, P&L, fees, Parquet export, and paired experiments remain Phase 2B. This ADR also
settles the RNG decision the architecture lists as ADR-0006.

## Versions and compatibility

- Engine version is 0.3.0. Replay still requires the recorded engine version to match.
- Run config schema 2 is schema 1 plus a required `scenario` object. A config without a
  scenario is still schema 1 and canonicalizes to the same bytes as before, so the Phase
  1B example keeps its config, book, and trade digests. Its command and trace digests
  change only because both log headers carry the engine version.
- The trace format stays at version 1. Scenario runs add record kinds
  (`FundamentalValueUpdate`, `ScenarioShock`) and a `scenario` object in the terminal
  record; scripted runs emit neither.

## Random numbers

- The generator is xoshiro256\*\*, seeded through SplitMix64. Both are checked against
  published vectors. The identifier `xoshiro256**-splitmix64-v1` is written into every
  scenario summary and must change if a generator or sampler changes.
- Every consumer owns a stream keyed by `(run seed, stream kind, stream ID)`: one for the
  fundamental and four per agent (policy, wakeup timing, latency jitter, value signal).
  A draw from one stream cannot move another, so replacing one agent's strategy leaves
  every other agent's random inputs untouched. That is the basis for paired comparisons.
- Samplers use integers only: mask-and-reject uniforms, a bitwise fixed-point logarithm
  for exponential waits, and a sum of twelve 16-bit uniforms for an approximate normal
  (exactly symmetric, bounded at six standard deviations). No `std::` distribution and
  no floating point are used anywhere in the engine. A seed therefore gives the same run
  on every toolchain, and the scenario golden digests are held across Apple Clang and
  GCC like the scripted ones. This is stronger than the same-platform guarantee the
  architecture requires.
- The final state of every stream is hashed into the summary as `rng_digest`.

## Events and ordering

- The scheduler's payload now also covers agent wakeups, market-data, order-report and
  fill deliveries, fundamental ticks, shock transitions, and periodic depth samples.
  Only exchange commands and exogenous outcomes are written to the logs. Wakeups and
  deliveries are not: they are regenerated on replay, and any difference in them shows
  up as a different command.
- At equal timestamps the scheduler sequence still decides. The queue is seeded with
  scripted commands, then shocks, the fundamental tick, sampling, and first wakeups in
  agent order. A shock that shares a timestamp with a fundamental tick therefore applies
  first, which makes shock intervals half-open: `[start, end)`.
- An agent may react within the timestamp of the event that prompted it when its latency
  is zero. Its message is scheduled at the current time with a later sequence, so it
  runs after everything already queued for that time. A delivery that lands on the same
  timestamp as an already-queued wakeup is seen at the next wakeup, not that one.
- Periodic samples reuse the snapshot command and its records.

## Observation boundary and latency

- A policy receives an `AgentObservation` and nothing else: logical time, the horizon,
  the public reference price, the latest market data *delivered to it*, its own working
  orders and position as known from *delivered* reports and fills, its limits and
  controls, and, for value agents, a noisy fundamental estimate. It has no handle on the
  book, the scheduler, other agents, or the latent value.
- Policies return intents. The environment turns an intent into a message only if it
  passes the agent's limits: order size, price range, open orders, a lifetime message
  budget, and worst-case position counting every working order on that side, including
  ones being canceled. Refused intents are counted per agent, not logged.
- The interface differs from the sketch in section 8.1 in one respect: callbacks append
  to a reused buffer through `AgentActions` instead of returning a vector, which avoids
  an allocation per callback. Report and fill callbacks carry no actions.
- Each agent has three delays: order entry (orders and cancels), reports (order reports
  and fills), and market data. Order entry may add uniform jitter from the agent's
  latency stream. Each path is first-in first-out per agent, so an agent's cancel cannot
  overtake the order it cancels; races between agents remain and are resolved by arrival.
- The exchange publishes one top-of-book update per command that traded or changed the
  best bid or ask. Every agent gets its own delivery event after its own delay.
- Agent order IDs are `(agent ID << 32) + n`. Scripted orders in a scenario must stay
  below 2^32, and agent IDs below 2^31.
- Summaries report each agent's fills and net position as recorded at the exchange. Net
  positions sum to zero across the parties to all trades. This is a share count, not the
  Phase 2B ledger: there is no cash, cost basis, or P&L yet, so the makers' cash
  constraint in section 8.2 is not enforced.

## Policies

Prices are integer ticks; internal sub-tick quantities are milliticks.

- Reference price for every policy: the delivered midpoint if both sides are quoted,
  else the last delivered trade price, else the scenario reference. This is the order
  section 10 gives for marks.
- **Market maker.** A ladder of `levels` quotes per side around the reference. It keeps
  a working quote that is at a wanted price and no larger than wanted (preserving queue
  position after partial fills), cancels the rest, and places what is missing within
  its position limit. Quotes round to the nearest tick with ties away from the
  reservation price, so the quoted spread is never inside twice the half spread.
- **Inventory-aware market maker.** The same quoting core with a shifted reservation
  price and a wider spread: `risk = variance * risk_aversion * remaining horizon
  fraction` in milliticks per share; the reservation moves against inventory by
  `position * risk`, and each side widens by `risk * quote_size / 2`. Variance is an
  integer EWMA of squared reference moves between wakeups with a configured floor. Both
  adjustments are clamped by `max_skew_ticks`. This has the shape of Avellaneda-Stoikov
  and is a stylized heuristic, not a calibrated implementation. With zero risk aversion
  it produces exactly the baseline maker's orders.
- **Momentum.** Samples the reference at each wakeup that has a market price, and after
  `long_window` samples sends an IOC limit through the touch in the direction of the
  short-minus-long average, subject to a threshold, a cooldown, and its position limit.
- **Value.** Compares the touch with its estimate and sends an IOC limit when the edge
  reaches the threshold. Size is one `order_size` per threshold of edge, up to its order
  limit. The limit price never gives up the threshold. The estimate is the latent value
  as of the wakeup plus normal noise from the agent's signal stream.
- **Noise.** Poisson arrivals. Each wakeup draws side, market-or-limit, size, and price
  offset in that order whether or not an order results, which keeps its stream aligned
  across runs. It caps its resting orders by canceling the oldest.

### Known property: quote-following makers have no price anchor

Makers quote around the market's own midpoint, so the price level is anchored only by
value traders. An inventory-skewed maker that is alone at the touch moves the midpoint
with its own quotes and then skews again from the new midpoint. With weak value flow and
a large skew clamp this walked the price by hundreds of ticks during a liquidity
withdrawal. The shipped scenarios keep it bounded with a two-tick skew clamp and value
demand that grows with mispricing. Experiments that change either should check the
distance between midpoint and fundamental before interpreting maker results.

## Fundamental value and shocks

- The latent value steps on a fixed interval:
  `value += (anchor - value) * mean_reversion_ppm / 1e6 + N(0,1) * volatility`, with
  truncating integer division and a clamp to `[1 tick, 4e12 milliticks]`. Each step is
  logged. Trade prices are never set from it.
- `FundamentalJump` shifts the value and its anchor at one instant. `LiquidityWithdrawal`
  scales the quote size of named market makers over an interval; zero makes them cancel
  everything. `VolatilityRegime` scales the step volatility over an interval. Each
  transition is logged with the shock's full definition.
- Overlapping volatility regimes, and overlapping withdrawals for one agent, are
  rejected at validation instead of being given a combination rule. Order-flow
  imbalance, latency degradation, and quote-withdrawal stress from section 9.3 are not
  implemented.

## Limits

Scenario values are bounded at validation so that no policy arithmetic can overflow 64
bits: prices to 1e12 ticks, positions and order sizes to 1e6 shares, durations to 1e15
ns, the horizon to 1e18 ns, at most 1,024 agents and 1,024 shocks. The existing event,
queue, and trace-byte caps apply unchanged and are the backstop against runaway
scenarios; the per-agent message budget bounds any single agent.

## Performance

Profiling the one-hour baseline showed SHA-256 at about 58 % of run time and JSON
construction at about 20 %; the scheduler, matching, and all agent logic together were
under 5 %. Two changes followed, neither of which alters a byte of output:

- SHA-256 hashes whole blocks straight from the input and uses ARMv8 SHA-2 instructions
  when the build targets them. The portable function is the fallback everywhere else and
  the reference in a test that holds the two to the same output.
- Record envelopes, trades, and order statuses are written directly as canonical text
  instead of through a JSON tree. A test parses every record of a run and requires
  re-serialization to reproduce it byte for byte.

Together they cut CPU time for a one-hour run with file output from 0.95 s to 0.34 s on
the reference machine, measured on the same config before and after. Market data fans out as one delivery event per agent
per update; that is exact and costs little next to trace hashing, so it was left alone.
See `docs/benchmarks/phase2a-baseline.md`.

## Verification

Known-answer and distribution tests for the generator; policy tests against hand-built
observations; strict scenario config parsing and bounds; integration tests for
determinism, seed sensitivity, share conservation, canonical form of every record,
latency and same-timestamp ordering, the delivery boundary, limits and caps, each shock,
and full replay with first-divergence reporting. The exit gate runs the shipped one-hour
baseline in the unit suite and through the CLI with replay, and pins its summary in
`core/tests/replay/golden/phase2a_baseline_summary.json`.
