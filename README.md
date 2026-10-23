# MarketTwin

Digital twin for market simulation.

MarketTwin is a research platform for studying how independent trading strategies interact inside a synthetic continuous double-auction equity exchange. The market-critical path is **C++20**: order matching, logical time, trading agents, accounting, experiment statistics, and deterministic event replay live in the native core. A Python service starts runs and serves what they recorded, and a browser terminal displays it. Neither computes a price, a fill, a P&L figure, or a statistic: they read the engine's artifacts.

There is no brokerage integration and no live order routing.

The complete technical contract is [`architecture.md`](architecture.md).

## Architecture

```text
Research terminal (React, TypeScript)   apps/web
        |  REST, same origin
Research API (FastAPI)                  services/api   registry in SQLite, jobs on a bounded pool
        |  one native call per run, experiment, or replay
markettwin (pybind11)                   bindings/python, python/markettwin
        |
C++20 simulation                        core   exchange, kernel, agents, scenarios, ledger,
        |                                      market statistics, experiments, trace, replay
        one logical thread per run
        event scheduler, matching engine, agents, accounting, telemetry

AI research tools                       (Phase 3B)
```

Subsystem boundaries (exchange, simulation kernel, agents, accounting, and telemetry implemented, with paired experiments in the native CLI; later subsystems planned):

| Area | Responsibility |
| --- | --- |
| Exchange | Order validation, price-time matching, cancels, executions |
| Simulation | Logical clock, event heap, latency, scenario dispatch |
| Agents | Strategy policies that emit intents, not book mutations |
| Accounting | Cash, inventory, P&L, fees |
| Telemetry | Event log, snapshots, replay digests |
| Bindings | One native call per run, experiment, or replay; JSON text in and out |
| Research | Paired experiments and statistics (in the core) |
| API | Validation, job control, and bounded queries over recorded artifacts |
| Web | Research terminal: configure, inspect, compare, verify |

A single run stays deterministic and logically single-threaded. Independent runs may later execute in separate processes. Kafka, Kubernetes, and other distributed infrastructure are out of scope for the MVP.

## Current status

**Phase 6 — timeline forks and the AI desk.** Pick any moment in a finished run and change
one thing from there on: add a shock, remove one that has not started, or switch a
participant's strategy. The engine replays the identical past, applies the change at a
barrier, and `fork-verify` proves the two runs agree record for record up to the fork,
then reports where they first part. An **AI desk** hands one agent to a language model:
every 60 s it sees only what that agent was delivered and returns a typed directive
(quote, pause, or reduce) that a native execution layer carries out. Every decision is
recorded, so replay never asks the model again, and the **decision ledger** forks the run
at each decision against the directive it replaced. A fork or a desk can be repeated
over paired seeds. See [ADR 0012](docs/adr/0012-timeline-forks-and-ai-desk.md) and
[the evidence](docs/experiments/phase6-desk/README.md).

**Phase 5B — calibration against observed sessions.** A pre-registered search fits
noise-trader, maker, and fundamental parameters so that twelve stylized facts of synthetic
runs (spread quantiles, trade arrival, return autocorrelation and volatility, quote state)
approach the same facts on observed AAPL IEX sessions, then tests the fit once on the most
recent sessions. Both sides go through one C++ L1 projection, and every fact and score is
an integer computed in the core. A null result is published as prominently as a pass. The
pipeline is verified offline on fabricated sessions; the real AAPL run needs Alpaca
credentials. See [ADR 0011](docs/adr/0011-calibration-methodology.md).

**Phase 5A — the remaining shock families.** Order-flow imbalance, latency degradation,
and quote-withdrawal stress join the fundamental jump, liquidity withdrawal, and
volatility regime. Each has a reference config, appears in the scenario editor, replays
byte for byte, and leaves every existing config and golden digest unchanged. See
[ADR 0010](docs/adr/0010-order-flow-latency-and-quote-stress-shocks.md).

**Phase 4B — live market data.** The API holds one upstream Alpaca stream (IEX, the
provider's test stream, or a local fabricated mock), normalizes every trade and L1 quote
in C++ with the Phase 4A conversions, and fans it out to the **Live Markets** workspace
over a bounded WebSocket protocol that announces every loss. A recording keeps every
provider frame verbatim and becomes an ordinary verified historical session per symbol.
Live observations never enter the synthetic exchange; there is no order entry.

**Phase 4A — observed historical equity data.** Fabricated offline L1 fixtures and
authorized Alpaca IEX/SIP records can be retained, normalized by C++ into exact
micro-dollar/UTC-nanosecond records, quality checked, replayed, verified, and inspected
in a separate Historical workspace. Historical observations never enter the synthetic
exchange and are not hypothetical executions.

**Phase 3A — bindings, research API, and the research terminal.** The engine can now be driven from a browser: configure a scenario, run it, scrub through its recorded market with price, depth, agents, and events moving together, compare strategies over paired seeds, and verify a recording by replay. See [The research terminal](#the-research-terminal).

**Phase 2B — accounting, market statistics, and paired experiments.** Every execution is posted to an integer double-entry ledger as it happens, each run publishes its books, a time series, and market microstructure statistics next to its trace, and replay verifies all of it. A batch runner compares strategy variants over matched seeds and reports paired differences with confidence intervals. The shipped comparison of a baseline and an inventory-aware market maker runs 400 one-hour markets in about two and a half minutes on one core of an M1 Pro.

Implemented now:

- C++20 library target `markettwin::core` and the `markettwin` CLI (`run`, `replay`, `experiment`, `--version`)
- Strong integer price, quantity, identity, timestamp, and sequence types
- Single-symbol `MatchingEngine` with ordered bid/ask maps, FIFO lists, and an ID index
- Limit GTC/IOC and market IOC orders, partial fills, and residual cancellation
- Structured execution/status reports, transactional rejection, and self-trade prevention
- Immutable depth snapshots, optional best quotes, and full book/index invariant checks
- Heap scheduler ordered by `(timestamp, scheduler sequence)` with an inclusive horizon, event and queue caps, and recorded stop reasons
- Strict versioned JSON run configs: unknown or missing fields, duplicate keys, floats, and out-of-range numbers are rejected
- Canonical `config.json`, `commands.jsonl`, `trace.jsonl`, `accounting.jsonl`, `metrics.json`, and a `manifest.json` with SHA-256 digests, published only after a completed run
- Replay that regenerates the run from its config and seed, byte-checks every command before dispatch, every output after it, and every ledger posting and sample, and checks final book and trade digests
- Trading policies: baseline market maker, inventory-aware market maker, momentum, value, and noise
- A delivered-observation boundary: per-agent order-entry, report, and market-data latency, with per-agent position, order-size, open-order, and message limits
- xoshiro256\*\* random streams per consumer with integer-only samplers, so a seed gives the same run on every toolchain
- A mean-reverting synthetic fundamental value and six shocks: fundamental jump, liquidity withdrawal, volatility regime, order-flow imbalance, latency degradation, and quote-withdrawal stress
- A per-agent ledger in integer minor units: cash, signed position, average-cost basis, realized and unrealized P&L, fees and rebates through a fee sink, with checked arithmetic and exact conservation of cash and shares
- Mark-to-market at the midpoint, else the last trade, else the scenario reference, with the source recorded; equity, peak, and drawdown on the sampling schedule
- Market statistics: spread, relative spread, top-of-book and N-level depth, volume, VWAP, fill rates, return volatility, exact time in each book state, rejections and cancels, and maker spread capture
- An optional per-agent credit limit enforced before a buy intent becomes an order
- A deterministic experiment runner: seeds times strategy variants, per-run failure reporting, output budgets, paired differences with Student t and bootstrap intervals in integer arithmetic
- A pybind11 extension, `markettwin._native`, with one call per run, experiment, or replay, the GIL released throughout, and `RunError` translated with its code and sequence
- A local research API: three-layer config validation, background runs and experiments with observed counts and cancellation, replay verification with the first divergence, and bounded, paginated queries over every recorded stream
- A research terminal with six screens: Overview, Simulation Lab, Market Microscope, Agent Inspector, Experiment Lab, and Replay Inspector
- Unit tests, 32 seeded randomized exchange traces checked against an independent reference matcher, policy and scenario tests, accounting and statistics tests, replay corruption tests, and golden fixtures held across Apple Clang and GCC
- Optional AddressSanitizer and UndefinedBehaviorSanitizer build
- Optional Google Benchmark target for exchange operations, ledger postings, statistics, and complete scenario runs

Not implemented:

- A cash check at the exchange: the credit limit is applied to an agent's intents, and a fill is never rejected for lack of cash
- Parquet artifacts, effective-spread and adverse-selection metrics, Sharpe ratios, and parallel execution of the cells of one experiment
- PostgreSQL, Redis, WebSocket progress, authentication, or any hosted deployment: the service is local and single-user
- Progress or cancellation inside a single run, and as-of series for VWAP, volatility, and time in each book state: the engine reports those only for a whole run
The scenarios are stylized. Prices come from the interaction of the configured agents, not from market data, and nothing here is calibrated to a real stock or says anything about real-world profitability.

## Running a simulation

```bash
./build/release/core/markettwin run --config configs/baseline.json --output data/baseline
./build/release/core/markettwin replay --input data/baseline
```

`run` prints the terminal summary as one JSON line and writes six files into a **new** directory. It refuses an existing directory, so a run is never overwritten or appended to.

| File | Contents |
| --- | --- |
| `config.json` | Canonical form of the input config; its digest is linked from both logs |
| `commands.jsonl` | Header, then each dispatched command in dispatch order (inputs to the exchange) |
| `trace.jsonl` | Header, then trades, order statuses, cancel results, snapshots, fundamental updates, shock transitions, and the terminal record |
| `accounting.jsonl` | Header with units and conventions, then one ledger entry per trade (both parties' balances after it) and one time-series sample per depth snapshot (top of book, depth, mark and its source, fundamental, and every agent's cash, position, basis, P&L, and equity) |
| `metrics.json` | Final per-agent books and order flow, market statistics, conservation totals, and the final mark, with units |
| `manifest.json` | Engine version, build provenance, terminal summary, the schema version of each stream, and artifact checksums; written last |

`replay` reruns the simulation from `config.json`, including every agent decision, requires each command to match `commands.jsonl` byte for byte before it reaches a fresh exchange, and compares each output with `trace.jsonl` and each ledger posting and sample with `accounting.jsonl`. It exits 0 and prints `"verified":true` when everything agrees, including `metrics.json`, the final book digest, trade digest, and file checksums. A difference in the books alone is reported as `AccountingMismatch` with the first differing record. On the first difference it exits 1 and reports the trace sequence, the expected and actual records, the causal command, and the last verified output. A run that failed part-way has no manifest and does not verify.

### Scenario configs

| Config | What it runs |
| --- | --- |
| `configs/baseline.json` | One simulated hour: two baseline makers, one inventory-aware maker, two value traders, two momentum traders, four noise traders |
| `configs/fundamental_shock.json` | The baseline with the fundamental dropping 150 ticks at 30 minutes |
| `configs/liquidity_shock.json` | The baseline with both baseline makers withdrawn from minute 20 to minute 30 |
| `configs/volatility_shock.json` | The baseline with fundamental volatility quadrupled from minute 20 to minute 40 |
| `configs/order_flow_shock.json` | The baseline with the four noise traders buying 80% of the time, 60% at market, at twice their rate from minute 20 to minute 30 |
| `configs/latency_shock.json` | The baseline with the three makers' orders slowed by 50 ms, reports by 20 ms, and market data by 50 ms from minute 20 to minute 30 |
| `configs/quote_stress_shock.json` | The baseline with all three makers canceling every working quote at minute 30 |
| `configs/baseline_accounting.json` | The baseline with a 30 ppm taker fee, a 20 ppm maker rebate, opening cash, and a zero credit limit on the three makers |
| `configs/phase1b_demo.json` | A scripted run with no agents: every order, cancel, snapshot, and stop comes from the config |
| `configs/experiments/maker_comparison.json` | An experiment, not a run: baseline versus inventory-aware maker over 200 seeds of the baseline |
| `configs/experiments/latency_shock_makers.json` | The same comparison over 100 seeds of the latency shock; report in `docs/experiments/phase5a-latency-shock/` |

The baseline and the six shock configs share a seed, so until its shock begins each variant is the same market as the baseline.

A scenario config (`schema_version` 2) is a scripted config plus a `scenario` object: a public reference price, the fundamental process, optional periodic depth sampling, the agents, and the shocks. Each agent has an ID registered with the exchange, a wakeup schedule (fixed interval or exponential), three latencies, hard limits, and a policy with its parameters. All values are integers: prices in ticks, sub-tick values in milliticks, times in nanoseconds, and fractions in parts per million. Malformed or out-of-range configs fail before the run starts.

A config that sets fees, opening cash, or credit limits is `schema_version` 3: the same fields plus an `accounting` object, with `scenario` allowed to be `null`. A config without one means no fees, zero opening cash, and unconstrained cash, and keeps the digest it had before Phase 2B.

The summary's `scenario` object reports, per agent, wakeups, orders and cancels sent, intents refused by the agent's own limits, fills, filled quantity, and net position in shares, along with the final fundamental value and a digest of every random stream's final state.

`configs/phase1b_demo.json` remains the reference scripted example: out-of-order scheduling, same-timestamp commands, a multi-level fill, a rejected foreign cancel, IOC residuals, an invalid order, and an explicit stop that leaves a later same-time order undispatched. Orders that are well-formed but invalid for the exchange, such as zero quantity or an unknown owner, run and are logged as rejections. The kernel assigns arrival time and sequence; configs cannot.

Golden summaries for the scripted demo and the baseline scenario are pinned in `core/tests/replay/golden/` and hold across Apple Clang and GCC builds.

[ADR 0002](docs/adr/0002-kernel-trace-and-replay.md) specifies ordering, stop conditions, the record format, digests, and replay failure codes. [ADR 0003](docs/adr/0003-agents-rng-latency-and-scenarios.md) specifies the agents, what they can observe, latency, random streams, the fundamental process, and shocks; [ADR 0010](docs/adr/0010-order-flow-latency-and-quote-stress-shocks.md) adds the Phase 5A shocks, and [ADR 0011](docs/adr/0011-calibration-methodology.md) the Phase 5B calibration. [ADR 0004](docs/adr/0004-ledger-marks-metrics-and-experiments.md) specifies the ledger, marks, market statistics, the accounting stream, and experiments.

## Accounting and metrics

Money is a signed 64-bit count of minor currency units (cents, say). A tick is worth `tick_size_minor` minor units, so a trade's notional is `price_ticks × quantity × tick_size_minor`, exactly. Nothing in the ledger, the valuation, or the statistics is floating point; every operation is checked, and an overflow fails the run instead of wrapping.

| Quantity | Definition |
| --- | --- |
| Cash | Opening cash, minus notional bought, plus notional sold, minus fees |
| Position | Signed shares; short positions and negative cash are allowed unless a credit limit is set |
| Cost basis | Total signed entry value of the open position (average cost, never rounded per share) |
| Realized P&L | On shares released: proceeds minus the basis released, gross of fees |
| Unrealized P&L | Position marked to market, minus cost basis |
| Equity | Cash plus marked position |
| Total P&L | Equity minus opening equity, which equals realized − fees + unrealized exactly |
| Max drawdown | Largest fall of equity from its running peak, over the start, every sample, and the end |
| Maker spread capture | Σ (price − midpoint the aggressor met) × quantity, signed in the maker's favor |

The mark is the midpoint when both sides are quoted, otherwise the last trade, otherwise the scenario's reference price, and `metrics.json` and every sample say which. The simulated fundamental is never a mark. A statistic with nothing to measure is `null`, not zero. The ledger is posted from the exchange's executions as they occur; what an agent knows of its own fills still arrives after its report latency.

Rounding happens in three places, always to nearest with ties away from zero: a fee per fill, the value of a position at a half-tick midpoint when the tick size is odd, and reported means and ratios (suffixed `milli`, `ppm`, `ppb`, or `millibps`).

## Experiments

```bash
./build/release/core/markettwin experiment \
  --config configs/experiments/maker_comparison.json \
  --output data/experiments/maker_comparison
```

An experiment config names a base run config, a list of seeds (or `{"first": 1, "count": 200}`), and strategy variants. A variant replaces the policy of named agents and nothing else. Every variant runs every seed, on one thread, in a fixed order. The command prints a table to the terminal, writes its results to a **new** directory, and exits non-zero if any run failed.

| File | Contents |
| --- | --- |
| `experiment.json` | Engine version and build provenance, the canonical experiment config with the base config embedded, every run's status and digests, per-variant completed and failed counts, and the paired comparison; written last |
| `runs.jsonl` | One line per run: its summary and its full metrics document |
| `runs/<variant>/seed-<n>/` | A complete, replayable run directory per cell, when the config asks for `"artifacts": "All"` |

With `"artifacts": "None"` each run's digests are recorded instead. Any cell can be regenerated by running the experiment with that one seed and `"All"`, and `markettwin replay` then verifies it against the same digests.

### The maker comparison

Agent 3 of the baseline scenario is the subject. One variant gives it the baseline `MarketMaker`, the other the `InventoryMarketMaker`, with the same half spread, quote size, levels, spacing, wakeup schedule, latency, and limits. The other ten agents are identical in both. A run contains one subject, never both.

Within a seed, the two runs share everything exogenous: the fundamental value's path, every agent's wakeup times, the value traders' signal noise, and the noise traders' draws. They do not share a market history. The subject's quotes change who trades with whom, what the other agents then see, and so everything after. The comparison removes exogenous noise; it does not hold the rest of the market still.

Results of the shipped config: 200 seeds, 400 one-hour runs, all completed, engine 0.4.0. Money is in minor units with `tick_size_minor` 1, so 100 units is one currency unit at a reference price of 15,000 ticks. Differences are inventory-aware minus baseline, paired by seed.

| Metric for the subject | Baseline mean | Inventory-aware mean | Mean difference | Median difference | SD of differences | 95 % t interval | 95 % bootstrap interval |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Total P&L (minor) | −130,985.7 | −85,376.6 | +45,609.1 | +41,252.0 | 86,084.2 | [33,605.7, 57,612.5] | [33,723.0, 57,379.4] |
| Max drawdown (minor) | 167,480.0 | 100,824.9 | −66,655.1 | −63,034.0 | 78,767.9 | [−77,638.4, −55,671.8] | [−77,552.6, −56,056.1] |
| Maker spread capture (minor) | 103,191.1 | 100,595.5 | −2,595.6 | −2,501.0 | 8,792.9 | [−3,821.7, −1,369.5] | [−3,781.2, −1,372.1] |
| Mean absolute position (shares) | 1,070.5 | 223.4 | −847.2 | −838.6 | 159.3 | [−869.4, −824.9] | [−869.5, −825.7] |
| Final absolute position (shares) | 1,122.9 | 1,084.1 | −38.8 | −14.5 | 628.9 | [−126.5, 48.9] | [−125.8, 49.3] |
| Market mean spread (ticks) | 3.297 | 3.157 | −0.140 | −0.141 | 0.031 | [−0.144, −0.136] | [−0.144, −0.136] |

What this does and does not show:

- **Neither maker makes money in this market.** The baseline maker ends a run with a profit in 7 seeds of 200 and the inventory-aware one in 1 of 200. Both earn about 1,000 currency units of spread per run and lose more than that on the inventory they are left holding.
- The inventory-aware maker loses less on average (the interval for the difference excludes zero), with a smaller drawdown and about a fifth of the average inventory. It does better than the baseline in 142 seeds of 200 and worse in 58. Its best run is also far smaller than the baseline's best.
- Final inventory is not distinguishable between the two. The control's pull weakens as the horizon approaches, by construction.
- The intervals describe variation across seeds of one stylized model with one parameter setting. They are not evidence about real markets or about profitability, nineteen metrics are reported with no multiplicity correction, and no Sharpe ratio is reported because a run yields one terminal P&L, not a return series.

The full table, including the thirteen metrics not shown here, is what the command prints.

## The research terminal

```bash
# 1. The API: builds the extension on first use, then serves on 127.0.0.1:8000.
cd services/api
uv sync
uv run python -m markettwin_api

# 2. The terminal, in a second shell: http://127.0.0.1:5173
cd apps/web
pnpm install
pnpm dev
```

Runs and experiments are written under `data/research/` (set `MARKETTWIN_WORKSPACE` to use another directory). The workspace starts empty and the terminal says so: nothing is shown until the engine has produced it.

| Screen | What it is for |
| --- | --- |
| Overview | What is in the workspace: scenarios, stored runs and experiments, active jobs, engine version |
| Simulation Lab | Choose a shipped scenario, edit exactly the fields the native config schema accepts, validate, and run |
| Market Microscope | One completed run. A timeline moves the price chart, the depth ladder, market telemetry, every agent's account, and the execution stream to the same recorded sample |
| Agent Inspector | One agent: inventory, equity, realized and unrealized P&L, drawdown, executed volume, and its ledger; optionally against another agent of the same run |
| Experiment Lab | A paired comparison: every metric's means, paired difference, and 95 % intervals, the per-seed distributions and scatter, the methodology, and any seed's two runs side by side |
| Replay Inspector | Verify a recording, read its digests, search its records, and, when it does not reproduce, see the first differing record field by field |

How the terminal treats data:

- **Time is elapsed simulation time.** No scenario defines a session clock, so no time of day appears. Wall-clock submission and processing times are shown separately and labelled.
- **A time resolves to the latest record at or before it.** Depth is never interpolated; the ladder names the snapshot it shows. Sampled series are drawn as steps.
- **Units are the engine's.** Prices in ticks, money in minor units, quantities in shares. No currency symbol is shown, because artifacts define only `tick_size_minor`.
- **Missing is not zero.** A value the run does not define is shown as unavailable.
- **Statistics are the engine's.** Means, intervals, and drawdown come from `metrics.json` and `experiment.json`. Three plotted series are derived from recorded ones and labelled so: the midpoint line, the drawdown curve, and cumulative executed volume. Tests hold each to the engine's own total.
- **No fallback data.** If the API is unreachable the terminal says so and shows nothing in its place.

Playback speed is a viewing pace (1× steps five recorded samples per second); the run itself finished before the page opened. An experiment that kept no run directories can still be inspected seed by seed: the terminal regenerates a cell and checks its digests against the experiment's record.

[ADR 0005](docs/adr/0005-bindings-api-and-terminal.md) specifies the binding, the API, what is derived, and the limits.

### Python

```python
import markettwin

summary = markettwin.run(open("configs/baseline.json").read(), "data/baseline")
verdict = markettwin.verify_replay("data/baseline")   # {"verified": True, "code": "Verified", ...}
```

`markettwin` exposes `run`, `verify_replay`, `canonicalize_config`, `canonicalize_experiment`, `experiment_cell_config`, `run_experiment`, `version`, and `build_info`. A failed run raises `markettwin.NativeRunError` with the engine's `code` and `sequence`.

## Historical observations

The bundled fixture is fabricated and safe for offline tests:

```bash
./build/debug/core/markettwin historical-import \
  --input core/tests/fixtures/historical/aapl_records.jsonl \
  --metadata core/tests/fixtures/historical/aapl_metadata.json \
  --output data/historical-aapl
./build/debug/core/markettwin historical-verify --input data/historical-aapl
```

From the terminal, open **Historical**, submit the local fixture, wait for validation,
then open the session to step through recorded trades and quotes. **Compare with
synthetic** shows definition-aware diagnostics against a completed simulation.

For authorized Alpaca access, copy `services/api/.env.example` into your local environment
setup and set `ALPACA_API_KEY_ID` and `ALPACA_API_SECRET_KEY`. The browser never receives
credentials. An Alpaca import requires an explicit UTC range, `iex` or `sip`, and the
entitlement/cost confirmation checkbox. Missing credentials do not affect offline mode.

Historical API routes are under `/api/v1/historical`: providers, datasets, imports,
sessions, bounded events/quotes/trades/series, quality, verification, and synthetic
comparison. Artifacts live below the configured workspace's `historical/` directory and
are untracked. See [ADR 0007](docs/adr/0007-observed-historical-market-data.md) for
ordering, schemas, licensing, and limitations. The reproducible fabricated-data
performance smoke is recorded in
[`docs/benchmarks/phase4a-historical.md`](docs/benchmarks/phase4a-historical.md).

## Timeline forks and the AI desk

In the terminal, open a finished run's **Timelines** tab, choose a time, a change, and
**Fork this run**. The fork's page shows the tree of timelines, both markets with the
shared past shaded and badged `PREFIX VERIFIED`, the gap between them over time, the
first divergence and first market divergence, and every agent's difference. **AI Desk**
in the top bar hands an agent to the desk; the run's **AI Desk** tab shows each decision
with its directive and the model's rationale, and runs the decision ledger.

From the CLI:

```bash
markettwin run --config DESK.json --output RUN --decisions fixture
echo '{"at_ns": 1500000000000, "add_shocks": [{"kind": "QuoteWithdrawalStress",
  "at_ns": 1500000000000, "agents": [1, 2]}]}' > stage.json
markettwin fork --parent RUN --stage stage.json --output FORK --decisions fixture
markettwin fork-verify --parent RUN --fork FORK   # also writes FORK/fork_report.json
```

A desk uses the configured OpenAI-compatible model (`MARKETTWIN_AI_MODEL`,
`MARKETTWIN_AI_API_KEY`) or the deterministic fixture, labelled `FIXTURE MODEL` wherever
it is shown. A model desk states how many calls it will make and starts only once exactly
that many are approved. Routes: `/api/v1/desks`, and under `/api/v1/runs/{id}`: `forks`,
`timeline`, `divergence`, `decisions`, `decision-ledger`, and `repeat`. A single fork is
`ONE SAMPLE PATH`; only a repeat over seeds supports a claim about an effect.

## Calibration

Offline, against fabricated sessions: open **Calibration**, **Import fabricated sessions**,
then **Run calibration**. Every screen labels the result `FABRICATED TEST DATA`.

Against observed AAPL IEX data (free on Alpaca's basic plan), with the Alpaca credentials
above:

```bash
cd services/api
uv run python scripts/calibrate_aapl.py --confirm-free-iex \
  --export ../../docs/experiments/phase5b-calibration
```

The script takes the ten most recent full regular sessions from Alpaca's trading calendar,
imports each 10:30–11:30 ET window through the Phase 4A path, runs the committed spec
[`configs/calibration/aapl_iex.json`](configs/calibration/aapl_iex.json) (257 candidates on
10 seeds, the best 10 on 50 fresh seeds, the selected one and the baseline on 100 test
seeds), verifies the artifact, and copies the aggregate report, per-session facts, spec,
and selected config to `--export`. Raw observed records stay in the untracked workspace.
The search is about 3,300 one-hour runs, roughly ten minutes on an 8-core laptop; on
macOS prefix the command with `caffeinate -i` so the machine does not sleep mid-run.
Each import and calibration row records the process that owns it, so starting or
restarting the API while the script runs leaves the script's jobs alone.

The committed run, for sessions 2026-10-07 to 2026-10-20, is a **null result**: the
selected config beats the baseline on test but is within two scales on 8 of the 12 facts,
not the required 9. See
[`docs/experiments/phase5b-calibration/`](docs/experiments/phase5b-calibration/README.md).

`markettwin calibration-facts --config RUN.json` prints the facts of one synthetic run, and
`--session DIR --start NS --end NS` those of a historical session. Calibration routes are
under `/api/v1/calibrations`: specs, create, list, status, report, candidates by stage,
selected config, verification, and the fabricated fixture sessions. Artifacts live below
the workspace's `calibrations/` directory.

## Live market data

The offline path uses the fabricated mock stream and needs no credentials:

```bash
cd services/api
uv run python -m markettwin_api.live_mock --port 8765 &
MARKETTWIN_LIVE_FEED=mock MARKETTWIN_LIVE_MOCK_URL=ws://127.0.0.1:8765/v2/mock \
  uv run python -m markettwin_api
```

Open **Live Markets**, choose **Mock**, and **Connect feed**. **Record** captures the
stream; **Stop recording** finalizes it into one historical session per symbol, listed in
**Historical** with its data label. With the Alpaca credentials above, choose **IEX** or
**Test** instead. Most Alpaca plans allow one stream connection, so the feed only starts
when asked (`MARKETTWIN_LIVE_AUTOSTART` is off) and the watchlist is capped at
`MARKETTWIN_LIVE_MAX_SYMBOLS` (30).

Live routes are under `/api/v1/live`: status, health, symbols, snapshot, feed start/stop,
recordings, and the `/stream` WebSocket. Controls are accepted from this machine or with
`MARKETTWIN_LIVE_CONTROL_TOKEN`. Captures live below the workspace's `live/` directory and
are untracked. See [ADR 0008](docs/adr/0008-live-market-data.md) for the gateway states,
clocks, ordering, bounds, protocol, and limitations.

## Native exchange API

Include `markettwin/exchange/matching_engine.hpp` and link `markettwin::core`.
An engine represents one symbol and owns its book for its lifetime. All commands
are synchronous on one logical thread. Inside a run the kernel supplies arrival
metadata; the example below drives the exchange directly.

```cpp
using namespace markettwin;
MatchingEngine exchange{ExchangeConfig{
    .symbol = SymbolId{1}, .agents = {AgentId{1}, AgentId{2}}}};

Order ask{
    .id = OrderId{1}, .owner = AgentId{1}, .symbol = SymbolId{1},
    .side = Side::Sell, .type = OrderType::Limit, .limit_price = PriceTicks{100},
    .original_quantity = Quantity{10}, .remaining_quantity = Quantity{10},
    .tif = TimeInForce::Gtc, .arrival_time = SimTime{0}, .arrival_sequence = Sequence{1}};
auto accepted = exchange.submit(ask);

Order buy{
    .id = OrderId{2}, .owner = AgentId{2}, .symbol = SymbolId{1},
    .side = Side::Buy, .type = OrderType::Market, .limit_price = PriceTicks{0},
    .original_quantity = Quantity{4}, .remaining_quantity = Quantity{4},
    .tif = TimeInForce::Ioc, .arrival_time = SimTime{0}, .arrival_sequence = Sequence{2}};
auto filled = exchange.submit(buy); // Four shares at the resting price of 100 ticks.
auto depth = exchange.snapshot(5);  // Ask: six shares at 100 ticks.
auto canceled = exchange.cancel(OrderId{1}, AgentId{1});
exchange.verify_invariants();
```

`ExecutionReport::incoming` holds the final incoming status and cumulative filled,
canceled, and resting quantities. `trades` and `order_updates` preserve match order;
maker updates precede the final incoming update. Rejections carry `RejectReason`
and leave the exchange unchanged. Quotes and order lookup return `std::optional`.

[ADR 0001](docs/adr/0001-exchange-semantics.md) specifies self-trade rejection,
accepted-ID uniqueness, input validation, ordering, resource limits, and failure
semantics. Arrival sequences must increase and timestamps cannot regress across
accepted submissions. Prices are integer ticks and quantity units are shares/lots;
no currency scale or ledger arithmetic is implied by these records.

`markettwin::Simulation` is the run boundary for native callers: construct it with a
`RunConfig` and an optional output directory, then call `run()`. Without a directory
it runs in memory and returns the same summary and digests.

A new trading policy implements `ITradingAgent` (`markettwin/agents/agent.hpp`). It
receives an `AgentObservation` and emits intents through `AgentActions`; it has no
access to the book, the scheduler, or other agents. Registering it means adding its
parameters to `PolicyConfig`, the factory and bounds in `core/src/simulation/scenario.cpp`,
and its JSON form in `core/src/telemetry/scenario_codec.cpp`.

## Prerequisites

- CMake 3.25 or newer
- Ninja
- A C++20 compiler: Apple Clang, Clang, or GCC
- Network access on the first configure, so CMake can fetch the pinned nlohmann/json and GoogleTest archives

Optional, for formatting and static analysis:

- `clang-format` and `clang-tidy` (LLVM 18 or newer)

For the research API and terminal:

- [uv](https://docs.astral.sh/uv/) and Python 3.11 or newer (the service pins 3.13)
- Node.js 22 or newer and [pnpm](https://pnpm.io/)

macOS:

```bash
brew install cmake ninja llvm
export PATH="$(brew --prefix llvm)/bin:$PATH"
```

Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y cmake ninja-build g++ clang-format clang-tidy
```

Check versions with `cmake --version`, `c++ --version`, and `clang-format --version`. `scripts/format.sh` and `scripts/lint.sh` search Homebrew LLVM and `/usr/lib/llvm-*` when the tools are not on `PATH`, and they exit with these install steps if the tools are missing.

## Build

Configure and build from the repository root. Builds stay out of source under `build/`.

```bash
cmake --preset debug
cmake --build --preset debug
```

Release:

```bash
cmake --preset release
cmake --build --preset release
```

Debug with AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
cmake --preset debug-sanitizers
cmake --build --preset debug-sanitizers
```

Without presets, the same Debug build is:

```bash
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/debug
```

`compile_commands.json` is written into the build directory.

The version command:

```bash
./build/debug/core/markettwin --version
```

Expected output:

```text
MarketTwin 0.4.0
```

`--help` prints usage. `run` and `replay` are described under [Running a simulation](#running-a-simulation) and `experiment` under [Experiments](#experiments). Any other argument exits with status 2.

## Tests

```bash
ctest --preset debug --output-on-failure
```

Equivalent after a manual configure:

```bash
ctest --test-dir build/debug --output-on-failure
```

Release and sanitizer presets have matching test presets (`release`, `debug-sanitizers`). Tests use GoogleTest and do not need a database, network service, or market data feed. Replay tests write to the system temporary directory and clean up after themselves. The suite includes two one-hour baseline runs (one in memory, one through the CLI with replay) and a small experiment through the CLI; a Debug build takes about 30 seconds in total.

Test layout:

- `core/tests/unit/exchange/` — matching, validation, snapshots, cancel lifecycle, and numeric bounds
- `core/tests/property/` — reference-model comparison, determinism, and per-order conservation after every action
- `core/tests/unit/simulation/` — scheduler ordering and bounds, strict config parsing for both schema versions, the fundamental process, the in-memory run boundary
- `core/tests/unit/agents/` — each policy against hand-built observations: quoting, inventory skew, warm-up, thresholds, limits, and random-draw alignment
- `core/tests/unit/common/` — SHA-256 standard vectors and accelerated/portable agreement; RNG published vectors, stream independence, and sampler distributions
- `core/tests/unit/accounting/` — checked money arithmetic at the 64-bit boundaries; opening, reducing, closing, and reversing long and short positions; partial fills, fees, duplicates, atomic rejection; the mark hierarchy, missing marks, and drawdown
- `core/tests/unit/telemetry/` — every market statistic against a small real exchange session, including empty and one-sided books and zero volume
- `core/tests/unit/experiment/` — means, medians, deviations, and intervals against hand-computed values; Student quantiles; bootstrap determinism
- `core/tests/replay/` — byte-identical reruns, corruption, truncation, tampering, and first-divergence reporting
- `core/tests/integration/` — scenario determinism, share conservation, canonical form of every record, latency and delivery boundaries, limits, shocks, scenario replay, golden summaries, and CLI run/replay tests; cash, share, and order-flow conservation, the cash constraint, config schema 3, accounting replay with first-divergence reporting; experiments: repeatability, paired exogenous processes, invalid designs, injected failures, output budgets, and a golden results document

The randomized tests print the seed and action number on failure. ASan/UBSan run
these same tests through the existing CI configuration.

The service and the terminal have their own suites. Neither mocks the engine.

```bash
cd services/api
uv run pytest            # binding and API tests against the real core, about 15 seconds
uv run ruff check . ../../python/markettwin && uv run ruff format --check . ../../python/markettwin
uv run mypy

cd apps/web
pnpm typecheck && pnpm lint && pnpm format
pnpm test                # unit and component tests
pnpm contrast            # WCAG contrast of every text token on every surface
pnpm e2e                 # starts the API and the terminal on spare ports and drives a browser
```

`pnpm e2e` needs a browser once: `pnpm exec playwright install chromium`. It runs a real one-hour simulation and a small experiment in a temporary workspace, compares what each panel shows at a time with what the API recorded there, verifies replay, audits every screen with axe, and cuts the API off to check the failure state.

- `services/api/tests/test_native.py` — the binding: golden parity with the CLI, error translation, concurrent runs, GIL release, experiment progress and cancellation, cell reproducibility
- `services/api/tests/test_runs.py` — validation layers, run provenance, every series against the recorded stream, as-of lookups, decimation, trades and ledger against the metrics, record paging and filters, shocks, scripted and failed runs, requests that try to leave the registry, the bounded queue, and recovery after an interrupted job
- `services/api/tests/test_replay.py` — verification, and what a changed trade, a changed ledger entry, another engine version, and a missing manifest each report
- `services/api/tests/test_experiments.py` — results equal to a direct native run, per-seed values, cell materialization with digest checks, live counts and cancellation
- `apps/web/src/**/*.test.ts(x)` — exact integer formatting, as-of lookup, the playback cursor, config editing, the table, the ladder, the interval plot against the engine's golden numbers, and failure states
- `apps/web/e2e/terminal.spec.ts` — one researcher's session end to end

## Benchmarks

Google Benchmark 1.9.4 is off by default, so a normal build does not download it. The target measures new-order, crossing, and cancel batches against the matching engine with fixture setup outside the timed region, ledger postings, account marking, paired statistics, and complete in-memory runs of the baseline scenario. CI runs it once as a smoke test and does not gate on timing.

```bash
cmake -S . -B build/benchmarks -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DMARKETTWIN_BUILD_BENCHMARKS=ON
cmake --build build/benchmarks --target markettwin_bench
./build/benchmarks/core/benchmarks/markettwin_bench
```

Stored baselines, with hardware, toolchain, workload, repetitions, and variance: [`docs/benchmarks/phase1b-baseline.md`](docs/benchmarks/phase1b-baseline.md) for the exchange [`docs/benchmarks/phase2a-baseline.md`](docs/benchmarks/phase2a-baseline.md) for scenario runs, and [`docs/benchmarks/phase2b-accounting.md`](docs/benchmarks/phase2b-accounting.md) for the cost of accounting against the Phase 2A binary. The figures are synthetic single-thread measurements, not a statement about live-exchange performance.

## Formatting and linting

```bash
scripts/format.sh          # fail if formatting differs
scripts/format.sh --fix    # rewrite sources
scripts/lint.sh build/debug
```

`scripts/lint.sh` reads `compile_commands.json` from the build directory you pass and checks the C++ sources listed there. Configure that build first. On macOS it passes the active SDK to Homebrew `clang-tidy`, which does not inherit Apple Clang's header lookup. The checked style is in `.clang-format`, `.clang-tidy`, and `.editorconfig`.

Naming used by the tidy config:

- Namespaces, functions, methods, variables: `snake_case`
- Types, structs, enums, enumerators: `PascalCase`
- Private members: `snake_case_`
- Files: `snake_case.hpp` / `snake_case.cpp`

## Layout

```text
architecture.md                 implementation contract
core/                           C++20 library, CLI, tests, benchmarks
bindings/python/                pybind11 extension source (markettwin._native)
python/                         the markettwin package and its build (scikit-build-core)
research/                       reserved for experiment notes and analysis
services/api/                   research API (markettwin_api) and its tests
apps/web/                       research terminal (React, TypeScript) and its tests
infra/                          reserved for local deployment files
configs/                       run configurations (scenarios and the scripted demo)
configs/experiments/           experiment configurations
schemas/                       reserved for published schemas
docs/adr/                      accepted implementation decisions
docs/benchmarks/               stored benchmark baselines
data/                          generated run output, untracked (data/research is the terminal's workspace)
```

`research/` is the placeholder from the Phase 0 brief and is still empty.

## Dependencies

| Dependency | Pin | When |
| --- | --- | --- |
| CMake | >= 3.25 | Always |
| nlohmann/json | 3.11.3, SHA256 pinned in `cmake/Dependencies.cmake` | Always (private to the core library) |
| GoogleTest | 1.15.2, SHA256 pinned in `cmake/Dependencies.cmake` | Tests, the default |
| Google Benchmark | 1.9.4, SHA256 pinned in `cmake/Dependencies.cmake` | `MARKETTWIN_BUILD_BENCHMARKS=ON` |
| pybind11 | 3.0.1, SHA256 pinned in `cmake/Dependencies.cmake` | `MARKETTWIN_BUILD_PYTHON=ON` |

The core needs nothing beyond CMake FetchContent. The service's Python dependencies (FastAPI, Pydantic, Uvicorn, HTTPX) are locked in `services/api/uv.lock`, and the terminal's packages in `apps/web/pnpm-lock.yaml`. PostgreSQL and Redis are intentionally absent.

Useful CMake options:

| Option | Default | Purpose |
| --- | --- | --- |
| `MARKETTWIN_BUILD_TESTS` | `ON` | GoogleTest and CTest |
| `MARKETTWIN_BUILD_BENCHMARKS` | `OFF` | Google Benchmark exchange target |
| `MARKETTWIN_BUILD_PYTHON` | `OFF` | The `markettwin._native` extension; set by `python/pyproject.toml` |
| `MARKETTWIN_ENABLE_SANITIZERS` | `OFF` | ASan and UBSan on MarketTwin targets |
| `MARKETTWIN_WARNINGS_AS_ERRORS` | `ON` | `-Werror` / `/WX` for MarketTwin targets |

## Phases

Status follows [`architecture.md`](architecture.md) section 20. Phases 1A through 3A, 4A, 4B, 5A, 5B, and 6 are implemented; Phase 3B is in progress. See [Phase 3B setup and evaluation](docs/phase3b-ai-research.md) and [ADR 0006](docs/adr/0006-supervised-ai-research.md).

| Phase | Scope | State |
| --- | --- | --- |
| 0 | CMake, CI, formatter, test runner, version CLI, no-op simulation type | Present in this tree |
| 1A | Order-book semantics and matching invariants | Implemented and locally verified |
| 1B | Event kernel, canonical trace, exchange replay | Implemented and locally verified |
| 2A | Trading agents, latency, shocks | Implemented and locally verified |
| 2B | Accounting, artifacts, paired experiments | Implemented and locally verified |
| 3A | pybind11, API, worker, research terminal, replay inspection | Implemented and locally verified |
| 3B | Supervised AI research workflow and institutional workspace | In progress |
| 4A | Historical equity ingestion and deterministic observed-market replay | Implemented; live provider access unverified |
| 4B | Live L1 market data, bounded fan-out, and recordings into historical sessions | Implemented against the mock stream; Alpaca IEX/test streams unverified |
| 5A | Order-flow imbalance, latency degradation, and quote-withdrawal stress shocks | Implemented and locally verified |
| 5B | Calibration of synthetic parameters to observed IEX stylized facts | Implemented; the AAPL IEX run is a null result |
| 6 | Timeline forks, the AI desk, and the decision ledger | Implemented and locally verified with the fixture model; a live model run awaits credentials |

## License

No license file has been added yet.
