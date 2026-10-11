# ADR 0004: Phase 2B ledger, marks, market statistics, and paired experiments

Date: 2026-10-10. Status: accepted.

## Scope

Implement architecture sections 10 and 14 natively, and the parts of 13 that do not need
a new dependency: a per-agent ledger, mark-to-market valuation, market microstructure
statistics, a per-run time series, a batch experiment runner, and paired statistical
comparison. This ADR also settles the decisions the architecture lists as ADR-0002 (money
units and overflow) and ADR-0007 (accounting convention and mark fallbacks). Python,
bindings, services, Parquet, and parallel execution of runs remain later work.

## Versions and compatibility

- Engine version is 0.4.0. Replay still requires the recorded engine version to match.
- **Trace format stays at version 1 and is byte-identical.** Accounting is written to its
  own stream with its own sequence, and nothing was added to the trace's terminal record
  or to `summary_json`. For the shipped scripted demo and the one-hour baseline,
  `commands.jsonl` and `trace.jsonl` produced by 0.3.0 and 0.4.0 differ in exactly one
  line each: the header, which carries the engine version. The golden summaries therefore
  keep their config, book, trade, and RNG digests and every count; only `commands_digest`
  and `trace_digest` changed, for that one reason, as they did between 0.2.0 and 0.3.0.
- Run config schema 3 is schema 2 plus a required `accounting` object, with `scenario`
  allowed to be `null` for a scripted run. A config whose accounting is the default (no
  fees, zero opening cash, no credit limits) canonicalizes as schema 1 or 2 with the
  bytes, and so the config digest, it always had.
- New, separately versioned schemas: accounting stream 1 (`accounting.jsonl`), metrics 1
  (`metrics.json`), experiment 1 (`experiment.json`, `runs.jsonl`). The run manifest is
  schema 2: it adds the two new files to `artifacts` and a `schemas` object naming the
  version of each stream. A schema 1 manifest is refused as `UnsupportedSchema`; it
  describes a run that has no books to verify.

## Units and arithmetic

- Money is a signed 64-bit count of **minor currency units** (`Money`). One tick is worth
  `tick_size_minor` minor units, so `notional = price_ticks * quantity * tick_size_minor`
  is an exact integer. Positions are signed 64-bit shares.
- There is no floating point in the ledger, the valuation, the market statistics, or the
  experiment statistics. Artifacts contain integers only.
- Every sum, difference, and product is checked. Products that are divided back into 64
  bits use a 128-bit intermediate (`__int128`, which both supported compilers provide).
  Overflow throws `RunError{"AccountingOverflow"}` and fails the run; nothing wraps and
  nothing saturates. A run fails this way only for values far outside any scenario's
  validated bounds, for example a scripted price of 2^62 ticks.
- There is one rounding rule, **nearest with ties away from zero**, and it is applied only
  where a value leaves exact arithmetic:
  1. a fee, per fill: `notional * rate_ppm / 1e6`;
  2. a marked value when the mark is a half-tick midpoint and the tick size is odd:
     `position * (bid + ask) * tick_size_minor / 2`;
  3. reported means and ratios, which carry a `milli`, `ppm`, `ppb`, or `millibps` suffix.
  One step truncates instead: cost basis released on a partial close (below), where the
  truncated remainder stays in the basis and so cannot leak P&L. The reported return
  volatility is the floor of a square root.

## Ledger

- **The ledger is posted from authoritative exchange executions**, inside the handler of
  the command that produced them, before anything is delivered to anyone. An agent's own
  view of its position and cash still arrives through its report latency, exactly as in
  Phase 2A. The two never mix: policies cannot read the ledger.
- Every agent registered with the exchange has an account, in scripted runs as well as
  scenarios. Accounts open flat with their configured cash (default zero). The `Ledger`
  class also accepts opening positions with a cost basis; run configs do not expose them.
- A trade debits the buyer and credits the seller the same notional, and moves the same
  shares the other way. A trade is booked into both accounts or neither: both updated
  accounts and the fee sink are computed on copies and then assigned.
- **Accounting basis: average cost**, carried as the total signed cost of the open
  position (`cost_basis`, positive long, negative short) so that no average is ever
  rounded. With `s` the signed shares of a fill (+ buy, − sell) and `p` its price in
  minor units per share:

  | Case | Effect |
  | --- | --- |
  | Opening, or adding to a position | `cost_basis += s * p` |
  | Reducing `c` of `\|q\|` shares | `released = cost_basis * c / \|q\|` (toward zero); `realized += -(s * p) - released`; `cost_basis -= released` |
  | Closing (`c = \|q\|`) | as reducing, with `released = cost_basis` exactly |
  | Reversing | the closing part as above, then the remainder opens at `p` |

  The same rows cover long and short positions. The identity
  `cash + cost_basis - realized_pnl + fees_paid = opening cash + opening basis` holds
  exactly after every fill, whatever was truncated.
- Realized P&L is gross of fees. Fees are tracked separately per account
  (`fees_paid`, negative when rebates exceed fees) and are not capitalized into cost.
- Fees are `maker_ppm` and `taker_ppm` of notional, each within ±1e6 ppm; a negative rate
  is a rebate. Every fee is paid to one **fee sink**, so
  `sum(cash) + fee_sink = sum(opening cash)` and `sum(position) = sum(opening position)`
  at all times. Both are verified, with each account's identity and share count, when a
  run finishes; a failure is `AccountingImbalance`.
- Trade IDs must arrive in exchange order from one. The next ID is booked; an ID already
  booked is ignored and reported as a duplicate; a later ID is a `TradeSequenceGap`
  error. The kernel treats a duplicate from the exchange as an internal error.
- Each account also keeps gross traded notional, shares bought and sold, fill count,
  maximum absolute position (updated on every fill, so it is exact), and minimum cash.

## Cash constraint

Phase 2A left the makers' cash constraint unenforced. Phase 2B adds an optional,
per-agent **credit limit**, enforced where position limits already are: by the
environment, before an intent becomes an order.

- With `credit_limit_minor` set for a scenario agent, a buy intent is dropped unless
  `delivered cash + credit limit >= cost of every working buy + cost of this one`.
  Cost is the full remaining quantity at the limit price plus the larger of the two fee
  rates. "Delivered cash" is the agent's cash according to fills it has been delivered,
  which is all it is allowed to know. Refusals are counted per agent (`cash_refusals`)
  and are also counted in the existing `intents_dropped`.
- For an agent that sends only limit orders, and with no fees, this is a guarantee: a
  fill is never worse than its limit, a fill is delivered before the report that shrinks
  the order, and sale proceeds count only once delivered, so authoritative cash cannot
  fall below the floor. With fees it holds to within rounding: each partial fill rounds
  its own fee, which can exceed the fee reserved on the whole order by up to half a
  minor unit per fill. A market order has no limit and is costed at the agent's
  reference price, which is an estimate and not a bound. The ledger reports each
  account's minimum cash, so any shortfall is visible.
- **A fill is never rejected or reversed for lack of cash.** By the time the ledger sees
  it the exchange has executed it against a counterparty.
- The default is no limit, and the shipped Phase 2A scenarios set none, so their runs
  are unchanged. A credit limit on an agent with no policy is rejected at validation,
  because scripted orders have no point at which it could be enforced.

**Not implemented, and proposed for approval before any work:** a strict check at the
exchange. It would need a risk gate between the command log and the matching engine that
holds authoritative cash, reserves the worst-case cost of every resting buy, releases
reservations on fills and cancels, bounds market orders with a price collar, and rejects
with a new reason code. That adds a reject reason to trace format 1 (a format bump and
new golden digests), changes which orders rest and so every scenario's calibration, and
makes exchange acceptance depend on accounting state. The environment-side check above
gives the makers' constraint without any of that.

## Marks and valuation

- A mark is a **market** price, taken from the exchange's own book at the moment of
  valuation. The simulated fundamental value is never a mark.
- Hierarchy, with the source recorded in every sample and in the final report:

  | Book and history | Mark | Source |
  | --- | --- | --- |
  | A bid and an ask rest | `(bid + ask) / 2` | `Midpoint` |
  | One-sided or empty book, at least one trade so far | last trade price | `LastTrade` |
  | Nothing has traded, scenario run | the scenario's public reference price | `Reference` |
  | Nothing has traded, scripted run | none | `None` |

  A lone bid or ask is not used: one side of a book is an offer, not a price at which the
  position could be closed. Marks are carried as twice the price in ticks so a half-tick
  midpoint is an integer.
- Per account: `market_value = position * mark`, `unrealized = market_value - cost_basis`,
  `equity = cash + market_value`, `total_pnl = equity - (opening cash + opening basis)`,
  which equals `realized - fees_paid + unrealized` exactly. A flat account needs no mark.
  An open position with no mark has no valuation, reported as `null`, not zero; inside a
  run this cannot happen, because a position implies a trade and a trade leaves a price.
- Equity is observed at the start of the run, at every sample, and at the end, each time
  at the mark of that moment. `peak_equity` and
  `max_drawdown = max(peak so far - equity)` are over those observations, in minor units.
  Drawdown therefore has the resolution of the sampling schedule and can miss an
  excursion between two samples. It is not expressed as a percentage, because equity may
  start at zero.
- Total P&L across accounts sums to minus the fee sink, up to half a minor unit per
  account when rule 2 above rounds.

## Market statistics

Collected by `MarketAnalytics` from what the exchange returns. It schedules nothing and
is handed copies, so observing a run cannot change it.

- **Exact, from every execution:** trade count, volume (each trade once), traded notional,
  VWAP, last, high, and low trade price.
- **Exact, from every command:** orders submitted, accepted, and rejected (by reason);
  cancels requested and succeeded; shares submitted, filled, expired (IOC and market
  residuals), and canceled. For every agent,
  `submitted = filled + canceled + expired + resting` over accepted orders.
- **Fill rates** are share-weighted with stated denominators: for an agent,
  `(maker + taker shares filled) / shares of its accepted orders`; for the market, both
  sides of every trade over all accepted shares, so `2 * volume / submitted`.
- **Exact logical time** the book spent two-sided, bid-only, ask-only, and empty, in ns,
  updated after every command. The four sum to the length of the run.
- **From the sampling schedule** (every depth snapshot in the trace, periodic or
  scripted): mean quoted spread and mean relative spread over two-sided samples; mean
  best-bid and best-ask size over the samples that have that side; mean resting shares
  in the first N levels of each side over all samples, an empty side counting as zero
  shares. No event was added or reordered to take these.
- **Return volatility:** the sample standard deviation (n − 1) of simple midpoint returns
  between consecutive two-sided samples, in parts per billion per sampling interval, not
  annualized. A one-sided sample breaks the chain.
- **Spread capture:** for each fill, `(price - midpoint) * quantity` signed in the
  maker's favor, against the midpoint the aggressing order met. It is positive for the
  maker and the same amount is the taker's cost. A fill that arrived at a one-sided book
  has no midpoint and is counted but not measured. This is the half spread earned at the
  moment of the trade; it says nothing about what the inventory was worth afterwards.
- **Missing is not zero.** A mean with no observations, a VWAP with no volume, a
  volatility with fewer than two returns or with a return beyond 1,000 % per interval,
  and depth means across mixed snapshot depths are `null`.

## Artifacts and replay

A run directory gains two files:

| File | Contents |
| --- | --- |
| `accounting.jsonl` | Header (units, fee and account config, agent and column order), one `LedgerEntry` per trade with both parties' balances after it, one `Sample` per depth snapshot (top of book, depth, mark and its source, fundamental, and each agent's cash, position, basis, realized and unrealized P&L, and equity), and a terminal record that commits to `metrics.json` by digest |
| `metrics.json` | Final per-agent books and order flow, market statistics, conservation totals, the final mark and its source, units, and the accounting conventions |

- Both are canonical JSON as in ADR 0002. `Sample` rows are arrays in a column order
  declared once in the header.
- The accounting stream has its own byte budget, equal to `max_trace_bytes`.
- Replay regenerates the run and byte-compares every accounting record as it is
  produced. A difference is `AccountingMismatch`, reported with the accounting sequence,
  the stored and recomputed record, and the last verified command. Because commands and
  executions are compared in the same pass, this code means the same market produced
  different books. `metrics.json` is compared whole (`MetricsMismatch`), and both files'
  checksums are in the manifest.
- The fundamental appears in `Sample` records for research. It was already public in the
  trace; no policy can read either file.
- Parquet export (architecture 13.2) is not implemented. The JSONL streams carry the same
  rows and need no dependency.

## Experiments

`markettwin experiment --config FILE --output NEW_DIRECTORY` runs every variant of a
scenario on every seed, on one thread, in a fixed order: seed-major, variants in config
order.

- A **variant** replaces the policy of named agents and nothing else. The agent keeps its
  ID, and with it its four random streams, its wakeup schedule, latency, and limits.
- Each cell's run config is the base config with the seed, the variant's policies, an
  optional shorter horizon, and `run_id = <experiment>-<variant>-s<seed>`. A cell is an
  ordinary run: the same config run alone gives the same digests.
- A cell that throws is recorded as failed with its code and message, and the experiment
  continues. Counts of completed and failed runs are reported per variant.
- `limits.max_runs` bounds seeds times variants, so a 200-seed, two-variant experiment
  must allow 400 runs. `artifacts: "All"` writes every run directory and needs
  `limits.max_output_bytes`; once the budget is spent the remaining cells fail with
  `OutputLimit` instead of filling the disk. With `"None"` each run's digests are
  recorded, and any single cell can be regenerated and replayed by running the experiment
  with that one seed.
- An existing output directory is refused. `experiment.json` is written last.

### The maker comparison

`configs/experiments/maker_comparison.json` compares a baseline market maker with the
inventory-aware one, over 200 seeds, on the shipped baseline scenario.

- **One participant is replaced.** Agent 3 is the subject. In variant `baseline_maker` it
  runs `MarketMaker`; in `inventory_maker` it runs `InventoryMarketMaker`, which is the
  shipped baseline scenario unchanged. The two are never in the same market as each
  other's subject: each run contains exactly one of them in that slot.
- **Parameters correspond.** Both use half spread 2 ticks, quote size 100, three levels,
  spacing 1 tick, the same wakeup phase and interval, the same latencies, and the same
  limits. The candidate adds only its inventory controls. A test holds this.
- **The other ten agents are identical** in both variants: two background makers, two
  value traders, two momentum traders, four noise traders.
- **Held constant across a pair:** the latent fundamental path, which no agent can
  affect; every agent's wakeup times; each value trader's signal noise; each noise
  trader's draws of side, type, size, and offset. These come from streams keyed by
  `(seed, stream kind, agent ID)`, and no draw from one moves another.
- **Not held constant:** everything downstream of the subject's quotes. The book, the
  trades, what other agents observe and so decide, and how many latency-jitter draws each
  agent has made all diverge. A paired seed removes exogenous variation from the
  comparison; it does not make the two market histories the same, and the difference
  includes how the rest of the market responded to the subject.

### Statistics

For each metric the paired difference is `candidate - baseline` on the same seed, over
the seeds both variants completed and for which the metric exists in both.

- Reported for the baseline, the candidate, and the difference: count, minimum, maximum,
  mean, median, and sample standard deviation (n − 1).
- **Student t interval** for the mean, two-sided 95 %:
  `mean ± t(0.975, n − 1) * s / sqrt(n)`. The quantile comes from a table for 1 to 30
  degrees of freedom and a four-term Cornish-Fisher series beyond, accurate to 1e-6. Its
  coverage assumes independent, normally distributed differences. Seeds give independent
  runs; P&L differences are not normal, and the interval is approximate.
- **Percentile bootstrap interval** for the mean difference, two-sided 95 %, 10,000
  resamples by default, from the k-th smallest to the k-th largest resampled mean with
  `k = floor(0.025 B) + 1`. It draws from `RngStream::Bootstrap`, keyed by the
  experiment's bootstrap seed and the metric's index, which no run ever draws from. It
  resamples the sorted differences, so it does not depend on run order.
- All of it is integer arithmetic, reported in thousandths of the metric's unit. Values
  must be within ±1e12 and there may be at most 100,000 of them; anything else is an
  explicit `StatisticsRange` error.
- Nineteen metrics are fixed in the code before any run and all are always reported:
  for the subject, total, realized, and unrealized P&L, fees, maximum drawdown, maker
  spread capture, final, maximum, and mean absolute position, traded quantity, and fill
  rate; for the market, trade count, volume, mean spread, mean relative spread, mean bid
  and ask depth, return volatility, and two-sided time. No multiplicity correction is
  applied, and the report says so.
- No Sharpe ratio is reported. A run yields one terminal P&L; a ratio would need a return
  series with a defended denominator and sampling interval, and equity that starts at
  zero has no return.
- The report labels a difference `higher`, `lower`, or `inconclusive` by whether the t
  interval excludes zero. That describes seed-to-seed variation inside one stylized
  model. It is not evidence that a strategy is realistic or profitable.

### Experiment artifacts

| File | Contents |
| --- | --- |
| `experiment.json` | Engine version and build provenance; the `results` document (the canonical experiment config with the base config embedded and seeds listed, per-run status, stop reason, and every digest, per-variant counts, and the comparison); `results_digest`; the checksum of `runs.jsonl` |
| `runs.jsonl` | One line per run: its summary and its full `metrics.json` document |
| `runs/<variant>/seed-<n>/` | A complete run directory per cell, when `artifacts` is `"All"` |

The `results` document contains no build information and no floating point, so its
digest is the same on every toolchain. A golden copy for a small experiment is held
across Apple Clang and GCC.

## Limits

Opening cash and credit limits are bounded to ±1e18 minor units and fee rates to ±1e6
ppm at validation. Experiments are bounded to 100,000 runs, names to 48 characters of
`[A-Za-z0-9_-]`, and bootstrap resamples to one million. The existing event, queue, and
trace-byte caps apply to every cell.

## Performance

Accounting adds one ledger posting and one short record per trade, two top-of-book reads
per order, and one record per sample that marks every account. The cost is dominated by
hashing and writing the extra bytes (about 6 MB beside 49 MB of logs for the one-hour
baseline). Measured against the Phase 2A binary on the reference machine, a one-hour
in-memory run went from 333 ms to 360 ms of CPU, about 8 % more; the ledger and valuation
arithmetic are about one millisecond of that. Details are in
`docs/benchmarks/phase2b-accounting.md`.

## Verification

Unit tests for checked arithmetic at the 64-bit boundaries, each of the four position
cases for longs and shorts, partial fills, fees and rebates, duplicate and missing
trades, atomic rejection, the mark hierarchy, missing marks, known drawdown sequences,
and every market statistic against a small real exchange session. Integration tests for
conservation of cash, shares, and order flow in a scenario; agreement of the ledger with
the Phase 2A share counts; reconstruction of drawdown and inventory from the published
series alone; the cash constraint; config schema 3; replay of the accounting stream with
first-divergence reporting; and experiments: repeatability, paired exogenous processes,
hand-checked paired differences, identical variants, invalid designs, injected failures,
output budgets, replay of every written run, and a golden results document. The
baseline hour's metrics are pinned in `core/tests/replay/golden/phase2b_baseline_metrics.json`.
