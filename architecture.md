# MarketTwin: Authoritative System Architecture and Multi-Agent Implementation Contract

> **Document:** `architecture.md` | **Status:** Implementation blueprint | **Version:** 1.0 | **Date:** 2026-10-10
>
> **Purpose:** The source of truth for coding agents and human maintainers building MarketTwin, a deterministic C++20 agent-based synthetic equity exchange and AI-powered quantitative research platform.
>
> **Primary constraint:** C++20 owns order matching, market state, logical time, trading agents, risk accounting, and deterministic execution from the very first commit. Python calls the C++ core at coarse-grained run boundaries only. No per-event calls into Python.
>
> **Scope:** Single synthetic equity symbol in the MVP, multiple independent trading agents, exact price-time matching, deterministic event replay, shock experiments, statistically sound comparison, a research API, a React dashboard, and a bounded AI research agent. No real trading or brokerage integration.

## 0. How to use this document

1. Read sections 1-12 before changing core architecture; read the applicable subsystem section and its contract before coding.
2. Treat all **MUST**, **MUST NOT**, and **SHALL** statements as hard constraints. **SHOULD** indicates a strong default; log exceptions in an ADR.
3. Implement only the current phase. Preserve future interface boundaries without building premature services.
4. Prefer working vertical slices with invariant tests over broad scaffolding.
5. Never silently replace a specified algorithm, ordering, schema, or accounting convention with a convenient alternative.
6. Every agent must finish with changed files, executed tests, known limitations, assumptions, and next recommended task.
7. No coding assistant should interpret this document as permission to trade or connect to external brokerage systems.

### 0.1 Decision hierarchy

1. Correctness and determinism.
2. Financial and simulation semantics.
3. Traceability and reproducible experimentation.
4. Secure and stable boundaries.
5. Runtime performance and resource efficiency.
6. Product polish and optional AI conveniences.

### 0.2 Definitions

- **MarketTwin:** the entire research platform.
- **Core:** a standalone C++20 library and CLI responsible for exchange, simulation, trading agents, accounting, and events.
- **Exchange:** the mechanism that validates/cancels/matches orders and publishes executions.
- **Kernel:** deterministic scheduler advancing logical time and dispatching events.
- **Agent:** a programmatic trader that observes permitted market information and sends orders.
- **Scenario:** a reproducible configuration of fundamental-price processes, agent populations, shocks, and latency.
- **Experiment:** a comparison across multiple simulations and strategies with explicit seeds and metrics.
- **Run:** a single execution of one scenario, strategy, and seed.
- **Event:** an immutable logical-time action or outcome, serialized in a versioned envelope.
- **Replay:** reconstruction from an authoritative ordered event record, with hashes and invariant validation.
- **AI researcher:** a bounded orchestrator creating typed experiment requests and interpreting computed outcomes, never a matching authority.

## 1. Product vision and non-goals

### 1.1 Product statement

Build a research-grade learning environment to study how independently acting trading strategies interact within a synthetic continuous double-auction exchange, and how liquidity, spreads, inventories, volatility, and simulated market impact behave under reproducible shocks. Users can design scenarios, execute comparisons, view results, inspect underlying event traces, and ask a supervised AI researcher to configure or interpret experiments.

### 1.2 Primary user journeys

1. Launch the system locally and execute a deterministic one-symbol run.
2. Inspect bids, asks, trades, agent inventory, cash, mark-to-market equity, and spread through time.
3. Run a baseline market-maker versus inventory-aware market-maker comparison under identical seeded scenarios.
4. Inject a fundamental-value jump, an order-flow imbalance, or a liquidity-withdrawal shock.
5. Repeat paired trials, compute effect sizes/confidence intervals, and inspect failed or unstable runs.
6. Replay a run and prove matching outputs and final accounting have not changed.
7. Use AI to convert a constrained English request into validated experiment configuration and grounded analysis.
8. Export report artifacts with version, seed, parameters, timestamps, metrics, and provenance.

### 1.3 Non-goals for the three-week MVP

- No production exchange certification, NASDAQ protocol compliance, regulatory-grade risk models, or live orders.
- No broker accounts, real money, investment recommendations, or operational execution routing.
- No derivatives, cross-asset arbitrage, market-wide routing, clearinghouse, or settlement simulation.
- No per-event distribution across nodes, consensus, distributed order-book ownership, or Kafka.
- No GPU reinforcement-learning training as a prerequisite.
- No claim that a stylized simulator predicts real market movements.
- No promise of realistic market calibration without independent empirical validation.
- No assumption that an LLM can calculate exact prices, P&L, confidence intervals, or matches.

### 1.4 Success criteria

- The C++ engine can process limit/market orders, partial fills, cancels, and price-time priority correctly.
- The simulator supports independent agents and coherent market-state transitions.
- A fixed run config, seed, source revision, compiler/toolchain policy, and event ordering produce matching result hashes on the specified platform.
- Event logs can be validated and replayed to equivalent final book and accounting state.
- Two market-making strategies can be compared over >=200 paired scenario seeds (configurable), reporting uncertainty and no cherry-picking.
- A REST API executes runs asynchronously without blocking its process.
- A UI displays run status, book depth snapshots, prices, spreads, inventory, P&L, and experiment comparisons.
- An AI agent can invoke a allowlisted experiment workflow and generate an evidence-linked report without mutating underlying financial numbers.

## 2. Architectural principles and boundaries

### 2.1 C++20 is authoritative

- Native code owns integer tick prices, quantity, order lifetime, crossing logic, execution price, trade IDs, event sequencing, logical time, all trading decisions for synthetic agents, position/cash updates, and trace hashes.
- Python may submit validated configurations and aggregate native outputs, but it MUST NOT inject discretionary per-event logic into a deterministic run.
- React never computes authoritative financial state. It displays server-provided numbers.
- AI may request experiments or interpret results, but cannot mutate run outcomes, bypass schema constraints, access future events, or submit live financial orders.

### 2.2 Separation of responsibilities

- `exchange`: input validation, book, matching, cancel/replace policy, executions.
- `simulation`: event heap, time, delivery latency, scenario initialization, event dispatch.
- `agents`: strategy policies, agent-specific observations, order/cancel intents.
- `accounting`: cash, inventory, realized and unrealized P&L, fee debits, checks.
- `telemetry`: compact event append, snapshots, counters, provenance and replay digests.
- `bindings`: coarse-grained run/config/summary transfer into Python.
- `research`: paired experiment design, statistical tests, analysis, reports.
- `api`: job management, access validation, query endpoints, streams.
- `web`: read-oriented operator UI.
- `ai`: schema-bound research requests and provenance-grounded explanations.

### 2.3 Explicitly chosen simplifications

- Start with one symbol and one native simulation thread per run.
- Integer tick price and integer share lots; define their unit scales in config.
- Resting orders are limit orders only; market orders are immediate-or-cancel by default.
- Market order residual is canceled, never posted.
- Limit order time-in-force defaults to GTC unless a future TIF is explicitly supported.
- An aggressive buy fills at resting sell price; an aggressive sell fills at resting buy price.
- Price-time priority is stable and independent of container iteration order.
- Partial fills preserve resting-order queue priority for remaining quantity.
- Execution/report delivery uses the kernel's event semantics, not direct privileged agent mutation.
- All pseudo-random draws use explicitly assigned engine state and stream IDs.

## 3. Full system topology

```text
Browser (React/TS)
  | REST queries / WebSocket progress
  v
FastAPI (validation/auth/status)
  |                          \
  | experiment jobs           \ bounded AI tools
  v                            v
Queue adapter (local -> Redis)  Research agent (Python)
  |                            | run/compare/inspect APIs only
  v                            |
Worker process ----------------+
  | pybind11 or native CLI
  v
C++20 Simulation (ONE LOGICAL THREAD PER RUN)
  +-- Event Scheduler / Clock
  +-- Exchange / Price-Time Matching / Order Book
  +-- Agent Environment + Policies
  +-- Market Scenarios + Shocks + Latency
  +-- Accounting and Deterministic Telemetry
  +-- Event Log / Snapshots / Checksums
  |
  +--> Parquet/columnar artifact export (offline conversion permitted)
  +--> Run summary -> PostgreSQL metadata

Independent worker processes run independent seeds in parallel.
```

### 3.1 Deployment profiles

- **P0 engine CLI:** CMake build, C++ tests, standalone binary, local config and artifacts. No runtime database.
- **P1 local research:** C++ binary, pybind11 bindings, Python notebooks/scripts, filesystem artifacts.
- **P2 app:** FastAPI, a worker, React, PostgreSQL, optional Redis in Docker Compose.
- **P3 optional later:** multiple worker replicas, storage service, authentication, stricter resource isolation; not MVP-critical.

### 3.2 Never add a network hop within matching

Agents, exchange, scheduler, and accounting stay inside the same native run. Network or Python boundaries occur before starting and after/between completed run artifacts. This protects ordering, performance, and replayability.

## 4. Monorepo structure

```text
markettwin/
  architecture.md
  README.md
  AGENTS.md
  CONTRIBUTING.md
  LICENSE
  .github/workflows/ci.yml
  cmake/                   toolchain.cmake, compiler_warnings.cmake
  core/
    CMakeLists.txt
    include/markettwin/
      common/{types,ids,errors,hash,version}.hpp
      exchange/{order,order_book,matching_engine,price_level,execution}.hpp
      simulation/{event,scheduler,kernel,clock,latency,scenario}.hpp
      agents/{agent,market_maker,momentum,value,noise}.hpp
      accounting/{ledger,positions,pnl,fees}.hpp
      telemetry/{event_log,snapshot,metrics,replay,manifest}.hpp
    src/{exchange,simulation,agents,accounting,telemetry}/
    apps/markettwin_cli.cpp
    tests/{unit,integration,property,replay}/
    benchmarks/
  bindings/python/
    CMakeLists.txt
    markettwin_bindings.cpp
  python/
    pyproject.toml
    markettwin/
      schemas/{experiment,run,results}.py
      orchestration/{runner,queue,worker}.py
      analytics/{compare,bootstrap,plots}.py
      ai/{agent,tools,prompts,evaluation}.py
  services/api/
    app/{main,config,deps}.py
    app/routes/{experiments,runs,artifacts,ws,ai}.py
    app/storage/{models,repositories}.py
    tests/
  apps/web/
    src/{pages,components,lib,hooks,types}/
    package.json
  infra/{docker-compose.yml,Dockerfile.api,Dockerfile.worker,Dockerfile.web}
  configs/{baseline,liquidity_shock,volatility_shock}.json
  schemas/{event_v1,run_config_v1,run_summary_v1,manifest_v1}.json
  docs/{adr,runbooks,experiments,benchmarks,diagrams}/
  scripts/{bootstrap,format,lint,run_smoke,verify_replay}.sh
  data/   # .gitignored generated run outputs
```

### 4.1 Code ownership

- Each subsystem has one unambiguous owner per task; parallel agents never edit the same implementation file concurrently.
- Shared schema changes require approval from the integration owner before dependent PRs begin.
- Architecture modifications require a dated ADR under `docs/adr/` and matching updates here.
- Changes to ordering, arithmetic scale, or replay format require compatibility tests and, when applicable, a schema-version bump.

## 5. Exact numeric and identity conventions

- Price is signed 64-bit integer ticks (`PriceTicks`), strictly positive for orders.
- Quantity is unsigned 64-bit shares/lots (`Quantity`), strictly positive for new orders.
- Simulated timestamps use unsigned 64-bit nanoseconds since run-local epoch; no wall-clock dependency.
- Sequence numbers use unsigned 64-bit values, monotonically increasing per run.
- `OrderId`, `TradeId`, `AgentId`, and `RunId` are strongly typed or alias-wrapped distinct integers/strings as appropriate.
- Tick size in dollars (example `$0.01`) is metadata, not floating-point engine state.
- Money arithmetic uses checked signed 128-bit intermediate arithmetic where needed; documented integer minor-unit ledger conventions are required.
- Fees are optional and deterministic. Define rounding and settlement units once in configuration.
- Overflow/underflow must yield explicit error, never wrap silently.
- Invalid user inputs must be rejected at the input boundary with structured reason codes.
- Every artifact records quantities and price units; do not infer units from column names.

## 6. Exchange and matching-engine contract

### 6.1 Order schema

```cpp
struct Order {
  OrderId id;
  AgentId owner;
  SymbolId symbol;
  Side side;                    // Buy | Sell
  OrderType type;              // Limit | Market
  PriceTicks limit_price;      // required for Limit
  Quantity original_quantity;
  Quantity remaining_quantity;
  TimeInForce tif;             // GTC | IOC for supported limit orders
  SimTime arrival_time;
  Sequence arrival_sequence;
};
```

### 6.2 Supported operations

- `submit(new_order) -> ExecutionReport` validates, matches aggressively, optionally rests the remainder.
- `cancel(order_id, owner) -> CancelResult` verifies ownership and resting existence.
- `snapshot(depth) -> OrderBookSnapshot` returns sorted bid/ask levels with aggregate quantity.
- `best_bid()`, `best_ask()` return optional values, not fabricated last prices.
- `verify_invariants()` checks the full order book in debug/test builds.
- Replace/amend is deferred. Initial behavior is cancel followed by new order, losing time priority.

### 6.3 Matching semantics

1. Check unique order ID, valid symbol, owner, quantity, price, TIF, and allowed limits.
2. Reject self-trades by a documented policy (MVP: cancel incoming conflicting quantity or reject incoming order; choose and test one default before agent implementation).
3. For buy limit, cross while best ask <= limit; for sell limit, cross while best bid >= limit.
4. For market orders, cross until requested quantity filled or opposite book exhausted.
5. At each level, execute oldest resting order first, at the resting order's limit price.
6. Decrement incoming and resting remaining quantities by identical fill quantity.
7. Remove fully filled resting orders and empty levels, update lookup index.
8. Rest eligible GTC limit remainder at its limit price, at the back of its FIFO level.
9. IOC and market residual is canceled; do not store unfilled market orders.
10. Emit normalized execution and order-status events with deterministic IDs and sequencing.

### 6.3.1 Phase 1A resolved policies

[ADR 0001](docs/adr/0001-exchange-semantics.md) defines the native exchange contract:
transactional whole-incoming-order self-trade rejection; lifetime uniqueness of
accepted IDs; configured symbol/agent validation and resource bounds; explicit
market IOC with zero unused price; caller-assigned monotone arrival timestamps and
sequences; checked level aggregates; immutable snapshots; exchange-local trade
ordinals and cumulative status quantities. The Phase 1B kernel (section 7.4) is the
sole caller: it stamps arrival metadata, and its dispatch order resolves exchange races.

### 6.4 Data structures

- Two ordered maps by price: bids descending, asks ascending; `std::map` with correct comparators.
- Each price level contains a stable FIFO doubly linked list of live orders (`std::list`).
- Global ID index maps `OrderId` to side/price/list iterator or another safe stable handle.
- Never leave stale iterators after fill/cancel; lifecycle tests are mandatory.
- Complexity goal: O(log L) level insertion, O(1) queue insertion at existing level, O(1) direct order removal plus O(log L) empty-level erase; trade processing scales with fills.
- Do not claim hard O(1) cancellation across all cases if level removal requires ordered map work.

### 6.5 Exchange invariants

- No crossed resting book after a completed submission (`best_bid < best_ask` if both exist).
- No zero-quantity resting orders, empty retained levels, duplicate active IDs, or orphaned ID-index entries.
- Within each level, arrival priority is FIFO by `(timestamp, sequence)`.
- Every fill quantity is positive and <= both sides' remaining quantities immediately before execution.
- Executed price equals resting price and conforms to aggressive order's limit, if any.
- Sum filled + canceled + resting for each order equals its original submitted quantity.
- Every trade has exactly one aggressor and one resting counterparty.
- Matching does not depend on hash-table iteration order.
- A rejected incoming order makes no partial state change unless documented transactional validation has already passed.

## 7. Simulation kernel and event ordering

### 7.1 Scheduler

- Event heap comparator orders by `(scheduled_ns, global_event_sequence)` ascending.
- The kernel advances clock only forward; no event may be scheduled before current time.
- Event IDs and trace sequence are deterministic and assigned centrally.
- Kernel dispatch is single-threaded within each run.
- Dynamic event insertion during dispatch is allowed at current or later timestamp with later global sequence.
- Explicit maximum event count, max time, queue capacity, and stop reason avoid infinite event loops.

### 7.2 Event categories

- `AgentWakeup`: strategy allowed to inspect latest permitted observation and generate intents.
- `OrderArrival`: previously sent order reaches exchange after configured latency.
- `CancelArrival`: cancellation reaches exchange; races resolved by event order.
- `Execution`: matching outcome produced by exchange.
- `OrderAcknowledgment`: agent receives accepted/rejected/new/canceled status.
- `MarketDataDelivery`: agent-visible state becomes available after configured dissemination latency.
- `FundamentalValueUpdate`: synthetic exogenous underlying value moves.
- `ScenarioShock`: modifies exogenous processes or agent parameters according to declared rules.
- `MetricSample`: collects periodic snapshots without altering matching behavior.
- `SimulationStop`: stops at configured horizon and emits final accounting.

### 7.3 Clock and latency

- All latency values are deterministic nonnegative logical-time delays.
- Separate order-entry, execution-report, and market-data dissemination delays.
- Latency distributions, if stochastic, are sampled from explicit agent-specific RNG streams.
- Agents only observe delivered data; the engine is not allowed to hand them future prices, other agents' private parameters, or undelivered fills.
- Zero-latency events at a common timestamp still have strict sequence order.
- The simulation should document whether event order allows an agent to react within the same timestamp and why.

### 7.4 Phase 1B resolved policies

[ADR 0002](docs/adr/0002-kernel-trace-and-replay.md) defines the implemented kernel:
a heap ordered by `(timestamp_ns, scheduler_sequence)`; a separate centrally allocated
trace sequence that orders commands and their outcomes in dispatch order and supplies
order arrival sequences and causation IDs; an inclusive horizon; and recorded terminal
reasons (`QueueEmpty`, `Horizon`, `EventLimit`, `Requested`). The Phase 1B payload set is
scripted submit, cancel, snapshot, and stop. The remaining 7.2 categories and all of
7.3 arrive with agents in Phase 2A.

### 7.5 Phase 2A resolved policies

[ADR 0003](docs/adr/0003-agents-rng-latency-and-scenarios.md) adds the remaining 7.2
categories as scheduler payloads. Exchange commands and exogenous outcomes (fundamental
updates, shock transitions) are logged; wakeups and deliveries are regenerated on replay.
Each agent has fixed order-entry, report, and market-data delays, with optional uniform
jitter on order entry drawn from the agent's latency stream; every path is FIFO per
agent. An agent with zero latency may react within the same timestamp: its message is
scheduled at the current time with a later scheduler sequence, so it follows everything
already queued for that time. The queue is seeded with scripted commands, then shocks,
the fundamental tick, sampling, and first wakeups in agent order.

## 8. Agents and behavioral model

### 8.1 Agent interface

```cpp
class ITradingAgent {
 public:
  virtual ~ITradingAgent() = default;
  virtual std::vector<Intent> on_market_data(const AgentObservation&) = 0;
  virtual std::vector<Intent> on_timer(const AgentObservation&) = 0;
  virtual void on_order_report(const OrderReport&) = 0;
  virtual void on_execution(const ExecutionReport&) = 0;
};
```

- Agent outputs are **intents**, never direct order-book mutations.
- Exchange ownership and agent ownership are separate.
- Observation includes logical time, delivered market data, own open orders, positions, cash, allowed risk limits, and optional public reference/fundamental signals.
- Agent interfaces shall not expose the kernel's heap, future random stream state, undisclosed orders, or other agents' balances.
- Agent event handlers must be deterministic functions of their state, observations, and own RNG.

### 8.2 Market maker (baseline)

- Posts a bid and ask around observed reference/midpoint, subject to valid tick rounding and min spread.
- Maintains at most configurable outstanding quote count per side.
- Cancels stale quotes before replacement; cancellation races are modeled explicitly.
- Respects cash, inventory, maximum order size, and minimum quote size.
- May optionally model spread selection from volatility and quote updates from observations.

### 8.3 Inventory-aware market maker

- Computes inventory-skewed reservation price and/or quote sizes from current signed inventory.
- Uses a documented stylized policy; distinguish heuristic control from validated Avellaneda-Stoikov assumptions.
- Inputs include inventory, volatility estimate, horizon, risk-aversion parameter, and observed mid/reference.
- Avoid pathological negative/invalid prices and overexposure via hard clamps.
- Evaluate against baseline under identical market-path random seeds.

### 8.4 Momentum agent

- Observes delivered price history, computes lagged short/long horizon signal, and trades with bounded inventory.
- Never uses future prices; feature lookbacks and warmup are explicit.
- Maintains cooldown and maximum active orders to control message volume.

### 8.5 Value agent

- Has a noisy estimate of synthetic fundamental value rather than omniscient true value unless scenario states otherwise.
- Trades when observed spread between price and estimate exceeds configured threshold/fees.
- Decays or resamples estimates according to an explicit process.

### 8.6 Noise liquidity agent

- Generates stochastic buy/sell order flow with documented arrival, side, size, and limit-offset distributions.
- Avoids concentration in zero-liquidity loops by configurable arrival rates and ranges.
- Uses independent RNG streams per agent.

### 8.7 Phase 2A resolved policies

[ADR 0003](docs/adr/0003-agents-rng-latency-and-scenarios.md) fixes the implemented
contract. The interface in 8.1 is kept except that callbacks append intents through an
`AgentActions` handle instead of returning a vector. The environment, not the policy,
owns each agent's delivered market data, working orders, position view, limits, and
random streams, and drops intents that would breach the agent's limits. Random numbers
come from xoshiro256\*\* streams keyed by seed, stream kind, and agent, sampled with
integer-only algorithms, so a seed gives identical runs across toolchains. Every policy
references the delivered midpoint, else the last delivered trade, else the scenario
reference. Agents track shares only; cash, cost basis, and the cash constraint in 8.2
arrive with the Phase 2B ledger (section 10.1).

## 9. Market scenarios and digital-twin fidelity

### 9.1 Synthetic reference price

- Maintain an exogenous latent fundamental price used for stylized value signals and shocks.
- Observed transaction prices remain **endogenous**: never overwrite traded price to a scripted target.
- A shock changes agent observations, valuations, order-flow distributions, or available liquidity; the actual book determines transactions.
- Distinguish last trade, midpoint, best bid, best ask, and latent fundamental; do not substitute one for another silently.

### 9.2 Baseline scenario

- One synthetic symbol; initial reference $150 with a configurable tick size.
- Market makers seed two-sided liquidity before aggressive noise orders.
- Momentum/value agents observe market data only after configured arrival latency.
- Run a fixed logical trading window with configurable number of agents and seed.

### 9.3 Shock families

- Fundamental jump downward/upward: adjust latent fundamental, observe repricing and inventory impacts.
- Liquidity withdrawal: reduce/disable a subset of maker agents or cap posted quote sizes for a bounded interval.
- Order-flow imbalance: shift arrival intensity and direction of aggressive orders.
- Volatility regime change: increase fundamental noise variance and selected agent response parameters.
- Latency degradation: raise order/cancel/feed delays for chosen agent classes.
- Quote withdrawal stress: sudden cancel intent surge with realistic arrival sequencing.

### 9.4 Calibration caveats

- Synthetic price distribution may not match real stocks; disclose this prominently.
- A successful result is comparative strategy behavior under an explicit simulated model, not proof of real-world profitability.
- At least one experiment should test sensitivity to parameter choices and random seeds.
- Show failed/null results and confidence intervals, not only the best-performing path.

### 9.5 Phase 2A resolved scenarios

[ADR 0003](docs/adr/0003-agents-rng-latency-and-scenarios.md) defines the fundamental as
an integer mean-reverting random walk in milliticks, logged at every step and never
written into a trade price. Implemented shocks are `FundamentalJump`,
`LiquidityWithdrawal`, and `VolatilityRegime`, each logged at every transition with
half-open `[start, end)` intervals; overlapping regimes or withdrawals for one agent are
rejected. Order-flow imbalance, latency degradation, and quote-withdrawal stress are not
implemented. Scenario runs use run config schema 2; scripted configs remain schema 1.
Phase 2B adds schema 3 for configs that set fees, opening cash, or credit limits.
`configs/baseline.json` is the reference one-hour market, with `fundamental_shock.json`,
`liquidity_shock.json`, and `volatility_shock.json` as single-shock variants of it.

## 10. Accounting, mark-to-market, and risk

- Every trade transfers shares and cash between counterparties at the same trade price and quantity.
- Buyer position increases, seller position decreases; cash debits/credits reconcile before fees.
- Agent cash and inventory start from configured balances and positions.
- Define whether shorting is allowed per agent. Enforce position and order exposure limits.
- Mark-to-market price policy: default mid when both sides exist, else last trade, else configured reference with a quality flag; document fallback.
- Realized P&L method: average-cost or FIFO, choose one explicit policy and test it.
- Unrealized P&L: signed open position times mark minus cost basis under chosen policy.
- Total equity = cash + marked inventory value; fees separately attributed.
- Track net P&L, realized/unrealized components, traded volume, gross exposure, max position, and drawdown.
- Cash/position conservation must hold across agents for every trade before external fees, funding, and exogenous transfers.
- Display negative cash or negative inventories only when policy allows leverage/short positions.

### 10.1 Phase 2B resolved policies

[ADR 0004](docs/adr/0004-ledger-marks-metrics-and-experiments.md) fixes the implemented
ledger. Money is a signed 64-bit count of minor units with `tick_size_minor` minor units
per tick, every operation is checked, and overflow fails the run. The ledger is posted
from authoritative exchange executions at the moment they occur; agents still learn of
their fills through report latency. The basis is average cost, carried as a total signed
cost so nothing is rounded until shares are released; realized P&L is gross of fees, and
fees and rebates go to a fee sink so cash and shares are conserved exactly. Marks follow
the order above (midpoint, last trade, scenario reference) with the source recorded, and
a one-sided quote is never a mark. Equity, peak, and drawdown are observed on the
sampling schedule. Short positions and negative cash are allowed by default. An optional
per-agent credit limit is enforced by the environment before a buy intent becomes an
order; a fill is never rejected for lack of cash. The headers are
`accounting/{money,fees,ledger,valuation,accounting_config,report}.hpp` in place of the
split sketched in section 4.

## 11. Deterministic event log, replay, and integrity

### 11.1 Why replay is part of MVP

A trustworthy market simulator must reproduce order-book transitions from an ordered authoritative record. Replay is not a visualization effect; it is a correctness feature enabling regression testing, failure diagnosis, and experimental comparability.

### 11.2 Logging model

- Record canonical inputs to matching as ordered events and canonical resulting output records.
- Store enough initialization state, configs, seeds, and schema/version metadata to reconstruct a run.
- Separate **command log** (inputs to exchange) from **execution trace** (accepted, rejected, fill, cancel, snapshot outcomes) to avoid double-applying results during replay.
- Record event timestamp, sequence, actor, event type, order ID, limit ticks, quantity, owner, and referenced causal event ID where applicable.
- Use integer canonical serialization with stable field ordering and explicit null representation.
- Generate incremental and final digests from canonical semantics, not process addresses or unordered-container iteration.
- Periodic book/ledger checkpoints speed debugging; they do not replace a complete source log.

### 11.3 Replay modes

1. **Exchange replay:** recreate book from canonical order and cancel inputs, compare execution trace and final book hash.
2. **Full deterministic simulation replay:** regenerate agent decisions from config/seeds/code version and compare entire event stream/hash.
3. **Trace inspection:** navigate persisted records and reconstruct snapshots for UI playback; no simulation authority.

### 11.4 Hash and portability policy

- Define a stable canonical byte layout, endian policy, hashing algorithm identifier, and version.
- Do not hash raw C++ structs with padding.
- Include schema version and config digest in manifests.
- Cross-platform bit-for-bit replay of floating-point stochastic calculations is **not guaranteed** unless a fixed math/toolchain policy is implemented. MVP must prove same-platform deterministic replay; cross-platform reproducibility is a stretch goal.
- Avoid serializing unstable unordered-map iteration and compiler-dependent `std::normal_distribution` outputs when portable replay is required. Prefer well-defined custom distribution algorithms or record random draws in trace.

### 11.5 Failure behavior

- Corrupted or incomplete logs fail verification with a precise sequence and reason.
- Schema incompatibility is explicit, not silently ignored.
- Replay divergence reports first differing event, preceding event context, expected/actual book state or trade, and associated run manifest.
- Event logs are append-only after finalization; a completed run manifest includes checksums for every artifact.

### 11.6 Phase 1B resolved format

[ADR 0002](docs/adr/0002-kernel-trace-and-replay.md) fixes format version 1: canonical
compact JSON with sorted keys and integer-only numbers, `config.json`, `commands.jsonl`,
`trace.jsonl`, and a `manifest.json` published last. Digests are SHA-256 over exact bytes.
Exchange replay (mode 1 of 11.3) is implemented and byte-compares every command before
dispatch and every output after it. Because Phase 1B has no floating-point or stochastic
calculation, its golden digests are also checked across toolchains. Parquet artifacts and
failed-run diagnostics manifests (section 13) remain Phase 2B work.

From Phase 2A ([ADR 0003](docs/adr/0003-agents-rng-latency-and-scenarios.md)) the same
verification is full deterministic simulation replay (mode 2 of 11.3) for scenario runs:
agent decisions are regenerated from the config and seed, and each resulting command must
match the command log before it reaches the exchange. The engine's stochastic
calculations are integer-only, so scenario golden digests are checked across toolchains
as well.

### 11.7 Phase 2B accounting replay

[ADR 0004](docs/adr/0004-ledger-marks-metrics-and-experiments.md) keeps trace format 1
byte-identical and adds a separate accounting stream with its own sequence and schema
version. Replay regenerates that stream alongside the commands and executions and
byte-compares every ledger posting and sample, reporting `AccountingMismatch` with the
first differing record, and compares `metrics.json` whole. The run manifest is schema 2
and lists the version of each stream. Engine version 0.4.0 changes only the header line
of `commands.jsonl` and `trace.jsonl`.

## 12. C++ API contracts and concurrency model

### 12.1 Native public interface

```cpp
struct RunConfig;
struct RunSummary;
struct RunArtifacts;
class Simulation {
 public:
  explicit Simulation(RunConfig config);
  RunSummary run();
  RunArtifacts artifacts() const;
};
```

- `Simulation::run` owns all in-run event dispatch, agent callbacks, and matching.
- API exposes config parsing/validation, summary, and artifact locations; not a per-event Python callback.
- An exception or structured error must include a stable code and useful context.
- RNG and event ordering are initialized once per run.
- A run never shares mutable exchange/agent state with another run.

### 12.2 Multicore use

- Parallelize across complete independent simulation runs, preferably separate worker processes.
- Do not parallelize matching of the same book in the MVP.
- If one worker supports multiple native runs, release the Python GIL around `run()` and validate thread safety; separate processes remain safer initially.
- Never infer deterministic scheduling from OS thread timing.

### 12.3 pybind11 boundary

- Transfer validated typed run configuration into the native engine.
- Return small summaries and immutable artifact paths/manifests.
- Avoid Python object creation per event, trade, or level in large runs.
- Translate native errors into stable Python exception types.
- Package with CMake/scikit-build-core or an equivalently reproducible wheel build.
- Treat C++ ABI and Python binding compatibility as separate versioned contracts.

### 12.4 Phase 3A resolved binding

[ADR 0005](docs/adr/0005-bindings-api-and-terminal.md) implements 12.3 as the extension
module `markettwin._native`, built with `-DMARKETTWIN_BUILD_PYTHON=ON` and packaged by
`python/pyproject.toml`. It was chosen over a CLI subprocess because the service needs a
failed run's code and sequence, counts from a running experiment, and cancellation. Each
function is one whole native operation exchanging JSON text; the GIL is released for the
whole call, and independent runs execute concurrently on threads because the core has no
global mutable state. `RunError` becomes `markettwin.NativeRunError` with its code and
sequence. The core gained one function, `experiment_paired_values_json`, which publishes
the per-seed values behind each comparison; no artifact, schema, or digest changed. A test
holds a run through the binding to the golden summary and metrics the CLI tests pin.

## 13. Artifact schemas and storage design

### 13.1 PostgreSQL (application metadata)

- `experiments(id, schema_version, config_json, status, created_at, completed_at, owner_id)`.
- `runs(id, experiment_id, strategy_key, seed, config_digest, status, artifact_uri, core_version, started_at, finished_at)`.
- `run_metrics(run_id, metric_name, numeric_value, units, definition_version)`.
- `research_reports(id, experiment_id, prompt_digest, result_manifest_uri, report_text, created_at)`.
- Use migrations and uniqueness constraints for idempotent jobs.
- No individual market event should be inserted into PostgreSQL during hot-loop processing.

### 13.2 Columnar artifacts

- `events.parquet` or canonical binary/JSONL event source plus Parquet derived table.
- `orders.parquet`: order IDs, owner, side, ticks, quantity, status transitions, simulation timestamps.
- `trades.parquet`: trade IDs, buy/sell agent IDs, aggressor side, ticks, quantity, fees, event sequence.
- `book_snapshots.parquet`: timestamp, sequence, depth level, side, ticks, aggregated quantity.
- `agent_positions.parquet`: timestamp, agent, cash, inventory, equity, realized/unrealized P&L.
- `metrics.json`: completed run statistics, units, calculation version, empty-book flags.
- `manifest.json`: config digest, seed, source revision, compiler flags, artifacts, hashes, status, runtime environment.

### 13.3 Storage lifecycle

- Outputs written to a temporary run directory and atomically renamed/finalized where supported.
- Failed runs persist a diagnostics manifest and partial artifact status.
- Retention limits and disk usage caps prevent runaway local storage.
- Data path never accepts unvalidated arbitrary user-provided filesystem locations.
- Large artifact downloads use bounded endpoints and, eventually, signed locations when remote object storage is introduced.

### 13.4 Phase 2B resolved artifacts

[ADR 0004](docs/adr/0004-ledger-marks-metrics-and-experiments.md) adds `accounting.jsonl`
(a ledger entry per trade and a time-series sample per depth snapshot) and `metrics.json`
(final per-agent books, order flow, and market statistics, with units and conventions)
to every run directory, both canonical integer-only JSON and both checksummed in the
manifest. Experiments write `experiment.json`, `runs.jsonl`, and optionally one run
directory per cell. Parquet tables are not implemented: the JSONL streams carry the same
rows without a new dependency. A failed single run still publishes no manifest; an
experiment records each failed cell with its error code.

### 13.5 Phase 3A resolved storage

[ADR 0005](docs/adr/0005-bindings-api-and-terminal.md) keeps the filesystem as the store
of record and replaces 13.1 with a single SQLite registry in the workspace
(`data/research/` by default): one row per run and per experiment, holding what the engine
does not record (who asked, wall-clock times, the last replay result). PostgreSQL is
deferred until more than one process needs the registry. The service never modifies an
engine artifact; the columnar index it builds for a run is written beside the workspace,
under `derived/`, and can be deleted and rebuilt. Request-supplied paths are not accepted
anywhere, and the only downloadable files are the six artifacts of a run.

## 14. Python research, experiments, and statistics

### 14.1 Experiment specification

- Explicit scenario name/version, strategy variants, base config, seed set, runs per variant, output metrics, compute budget.
- Paired comparison uses same independent scenario randomness where meaningful, with strategy-dependent RNG separated to reduce confounding.
- Do not accidentally run 200 total when the plan says 200 seeds **per strategy**.
- Metrics require units, sample definitions, aggregation policy, and missing-value policy.
- Use mean/median, standard deviation, percentile intervals, effect sizes, and bootstrapped confidence intervals when appropriate.
- Report assumptions about dependence among runs and whether seeds represent independent trials.
- Always preserve raw results, seeds, failed-run counts, and filtering decisions.

### 14.2 Primary quant metrics

- Quoted and effective spread (define spread units and aggregation windows).
- Depth at best bid/ask and first N price levels.
- Trade count, traded shares, aggressive-side distribution, volume per agent class.
- Midpoint volatility computed only at valid sampling points.
- Fill ratio, maker participation, maker inventory time series.
- Net/realized/unrealized P&L and fees.
- Max drawdown and peak position (define mark and time sampling).
- Adverse-selection proxy based on forward midpoint movement after fills, with explicit horizon.
- Implementation shortfall only when order-arrival benchmark and unfilled cost policies are specified.

### 14.3 Comparison discipline

- Choose metrics before running headline comparison.
- Prefer paired differences with the same scenario seed over comparing unrelated samples.
- Show distributions and downside tails, not only averages.
- Check scenario validity (e.g. minimum transactions, at least intermittent two-sided liquidity).
- If outcomes are unstable, report instability and adjust environment validity rather than optimizing away bad seeds.

### 14.4 Phase 2B resolved experiments

[ADR 0004](docs/adr/0004-ledger-marks-metrics-and-experiments.md) implements 14.1 to 14.3
natively, in the CLI, ahead of the Python layer. An experiment runs every variant on
every seed; a variant replaces named agents' policies and nothing else, so paired runs
share the fundamental path and every agent's own random streams while the market that
follows is free to diverge. Paired differences are reported with mean, median, standard
deviation, a 95 % Student t interval, and a 95 % percentile bootstrap interval, all in
integer arithmetic, with failed runs and undefined metrics counted and never filled in.
The metric list is fixed in code before any run. Effective spread, adverse-selection
proxies, and implementation shortfall from 14.2 are not implemented; maker spread capture
against the pre-trade midpoint is.

## 15. API and job orchestration

### 15.1 REST endpoints

- `POST /api/v1/experiments`: validate and create experiment; return ID/status.
- `GET /api/v1/experiments/{id}`: metadata and progress.
- `GET /api/v1/experiments/{id}/comparison`: paired statistics and provenance.
- `GET /api/v1/runs/{id}`: run config, summary, artifact index.
- `GET /api/v1/runs/{id}/book?time_ns=&depth=`: bounded persisted snapshot query.
- `GET /api/v1/runs/{id}/trades?offset=&limit=`: paginated trades.
- `GET /api/v1/runs/{id}/agents`: agent accounting summaries.
- `POST /api/v1/runs/{id}/verify-replay`: initiate/retrieve replay-verification job.
- `POST /api/v1/ai/research`: schema-limited request to generate or interpret experiment.
- `GET /api/v1/health` and `/api/v1/ready`: service and dependency checks.

### 15.2 WebSocket

- `/api/v1/ws/experiments/{id}` publishes bounded progress messages and completion events.
- WebSocket messages are not authoritative market transactions; avoid streaming every native event.
- UI can fetch sampled snapshots or paginated traces for replay.
- Reconnection resumes from stored job state, not in-memory client assumptions.

### 15.3 Background workers

- Phase 1 CLI works without queue/database.
- Phase 2 worker can start as a local Python process and SQLite/file state if needed.
- Add Redis-backed queue only after queue semantics matter; exact implementation (RQ, Celery, Arq, etc.) is an ADR decision.
- Jobs are idempotent by run ID + config digest; repeated enqueues do not overwrite completed runs.
- Workers enforce CPU, run duration, event count, memory, and disk budgets.
- Cancellation is cooperative at safe simulation checkpoints; final status is explicit.

### 15.4 Phase 3A resolved API and worker

[ADR 0005](docs/adr/0005-bindings-api-and-terminal.md) implements 15.1 and 15.3 as a
FastAPI service, `services/api/markettwin_api`, bound to the loopback interface. Jobs run
on a bounded thread pool inside the API process, not a queue: two at a time, sixteen
waiting, then `429`. A job interrupted by a restart is recorded as failed. Endpoints, all
under `/api/v1`:

| Area | Endpoints |
| --- | --- |
| Service | `GET /health` |
| Scenarios | `GET /scenarios`, `GET /scenarios/{key}`, `GET /schema/fields` |
| Validation | `POST /configs/validate`, `POST /experiments/validate` |
| Runs | `POST /runs`, `GET /runs`, `GET /runs/{id}`, `DELETE /runs/{id}` |
| Recorded series | `GET /runs/{id}/samples/times`, `/samples`, `/series/market`, `/series/agents/{agent}`, `/book` |
| Records | `GET /runs/{id}/trades`, `/events`, `/agents/{agent}/ledger`, `/artifacts/{name}` |
| Replay | `POST /runs/{id}/replay`, `GET /runs/{id}/replay` |
| Experiments | `POST /experiments`, `GET /experiments`, `GET /experiments/{id}`, `/results`, `/paired-values`, `POST /experiments/{id}/cancel`, `POST /experiments/{id}/cells/materialize` |

These differ from 15.1 in three ways. The comparison is served as the engine's whole
results document (`/results`) with the per-seed values beside it. Depth is served both by
time (`/book`) and in windows of exact samples (`/samples`), because the terminal's cursor
is a sample index. Progress is polled; the WebSocket of 15.2 and the health/ready split
are not implemented. Validation reports the schema's, the service's, and the engine's
errors separately. A run reports no progress, because the core runs it as one call; an
experiment reports the count of finished runs.

## 16. AI quantitative research assistant

### 16.1 Allowed capabilities

- Translate English into validated scenario/strategy parameters.
- List existing experiments and compare completed results.
- Request a bounded experiment batch after user-visible plan confirmation where needed.
- Inspect metric definitions, sample sizes, seed coverage, and artifacts.
- Generate summaries with citations to run IDs, metric values, and manifest versions.
- Propose follow-up experiments and uncertainty checks.

### 16.2 Forbidden behaviors

- No access to a real broker, brokerage API, or financial execution account.
- No direct database writes bypassing typed research tools.
- No arbitrary shell commands or unrestricted arbitrary Python code execution from LLM output.
- No direct file path traversal or exfiltration of local configuration/secrets.
- No fabricating computed values; all numeric conclusions must be sourced from tool results.
- No retroactive changing of completed run metrics or event logs.
- No reliance on LLM reasoning for order matching, numerical portfolio/accounting calculations, or confidence intervals.

### 16.3 Tool schemas

- `list_scenarios()` returns allowlisted scenario keys and parameter bounds.
- `validate_experiment(spec)` returns typed validated spec and estimated cost.
- `start_experiment(spec)` returns job ID, accepted config digest, and limits.
- `get_experiment_status(experiment_id)` returns progress/failure count.
- `get_comparison(experiment_id)` returns structured metrics and confidence intervals.
- `get_run_trace(run_id, start_seq, limit)` returns bounded causal records.
- `verify_replay(run_id)` returns verification status and mismatch context.
- `generate_report(experiment_id, analysis_spec)` produces references to immutable results.

### 16.4 Evaluation

- Fixed benchmark prompts with expected tool sequences and validation outcomes.
- Numerical grounding checks compare report values against structured tool output.
- Permission tests attempt oversized runs, forbidden operations, path traversal, and nonexistent run IDs.
- Ambiguous request tests require clarification or safe default, never invented strategy behavior.
- Hallucination score includes invented observations, unjustified causal explanations, and missing caveats.
- Tool-call trace and model version are recorded without persisting secrets.

## 17. React research console

### 17.1 Routes

- `/` project overview, health and recent runs.
- `/experiments/new` scenario builder and seed/strategy configuration.
- `/experiments/:id` job status, comparison statistics, downloadable artifacts.
- `/runs/:id` time series, order book depth, trade tape, agent positions, mark-to-market P&L.
- `/runs/:id/replay` scrubber over event time with validation status.
- `/research` AI panel for constrained experiment planning and evidence-linked reports.

### 17.2 Presentation correctness

- Every chart shows axis labels, units, sample interval, and source/run IDs.
- Display `N/A` when no two-sided book exists rather than inventing a spread/midpoint.
- Differentiate simulated fundamental, last trade, and midpoint prices.
- Show run seed, scenario, strategy revision, simulation duration, and fidelity disclaimer.
- User can identify incomplete/failed runs and inspect reasons.
- Replay timeline uses persisted events/snapshots and never alters run source data.
- Avoid chart downsampling methods that obscure extreme market events without disclosure.

### 17.3 Phase 3A resolved terminal

[ADR 0005](docs/adr/0005-bindings-api-and-terminal.md) implements the console as the
MarketTwin research terminal in `apps/web`. Routes:

| Route | Screen |
| --- | --- |
| `/` | Research overview |
| `/simulations`, `/simulations/new` | Stored runs; Simulation Lab |
| `/runs/:id` | Market Microscope |
| `/runs/:id/agents/:agent` | Agent Inspector |
| `/runs/:id/replay`, `/replay` | Replay Inspector; verification of stored runs |
| `/experiments`, `/experiments/new`, `/experiments/:id` | Stored experiments; configurator; Experiment Lab |
| `/experiments/:id/seeds/:seed` | One seed's baseline and candidate runs, side by side |

The replay screens of 17.1 are part of this phase; the AI panel is not. Every chart has an
elapsed-simulation-time axis with units, sampled series are drawn as steps, and a time
resolves to the latest record at or before it. A missing value is shown as unavailable,
never as zero. The terminal formats, plots, and bins; it computes no P&L, statistic, or
interval, and when the API does not answer it shows nothing in its place.

## 18. Security and operational safety

- No brokerage credentials or live exchange network endpoints are part of the codebase.
- Configuration inputs are schema validated with bounded numeric ranges and enumerated strategy types.
- Do not compile/execute code supplied by a remote user through AI prompts.
- Use explicit filesystem roots for artifacts; normalize and validate paths.
- Cap request size, concurrent jobs, CPU, event count, memory, and artifact disk usage.
- Protect provider API secrets through environment variables, never tracked files or logs.
- Treat AI input and fetched documents as untrusted; they cannot override system tool policies.
- Implement minimal local access controls now; design ownership semantics for future hosted deployment.
- Data provenance and code version are required in experiment reports.

## 19. Build, dependency, CI and quality strategy

### 19.1 Build tooling

- CMake >=3.25 target (adjust via ADR when necessary), C++20, GCC/Clang in CI.
- C++ warnings enabled and treated as errors for project-owned core code where practical.
- GoogleTest or Catch2 for unit/integration; choose one as the canonical framework.
- Google Benchmark for throughput/latency microbenchmarks.
- clang-format and clang-tidy configured consistently.
- Sanitizer profiles: AddressSanitizer + UBSan in CI; ThreadSanitizer only if concurrency is introduced.
- Python package built using pybind11 and scikit-build-core.
- pytest for Python/API/AI tests, ruff for style; mypy optional at typed boundaries.
- ESLint/TypeScript strict checking and component tests for web.

### 19.2 CI gates

- Configure/build C++ core in release and sanitization profiles.
- Run unit tests, integration tests, deterministic golden traces, and replay verification.
- Run minimal native benchmark smoke, but do not gate CI on noisy latency thresholds.
- Package/import Python native binding and run Python integration tests.
- Validate schema examples and generated artifacts.
- Run frontend lint/typecheck/build when frontend exists.
- Block merges on incompatible event/schema changes without migration/tests.

### 19.3 Performance measurement policy

- Keep test workloads representative: mostly resting, mostly crossing, cancel-heavy, shock bursts, deep/shallow books.
- Report hardware, compiler, optimization, build hash, input distribution, warmup, repetitions, and variance.
- Separate order submission latency from matching fills per order and complete simulation runtime.
- Define p50/p95/p99 units and measurement population.
- Profile before optimizing; use allocation/event counts and memory peak metrics.
- Never report synthetic throughput as live-exchange capability.

## 20. Engineering phases and exit gates

### Phase 0: Repo and tooling (Day 0-1)

- Initialize CMake/C++20 project, CI, formatter, test runner, architecture file, contributor conventions.
- Select canonical build/test commands and pin dependency versions or reproducible acquisition policy.
- Create C++ CLI and compile a no-op deterministic `Simulation` skeleton.
- Exit: clean clone builds and tests on primary development platform; CI works.

### Phase 1A: Core order-book semantics (Days 1-3)

- Implement strong numeric types, validated orders, bid/ask structures, ID index, price-time matching.
- Support partial fills, market residual cancellation, GTC rest, IOC, ownership-aware cancels.
- Test crossing, FIFO, empty levels, cancel races at pure exchange level, overflow, invalid inputs.
- Exit: matching invariant suite green under sanitizers and repeated randomized action traces.

### Phase 1B: Kernel and trace (Days 4-7)

- Implement heap scheduler, logical clock, dispatch, event sequencing, basic CLI config.
- Integrate exchange as sole order consumer; append canonical command/output trace.
- Implement exchange replay with final book/trade digest checks and first-divergence diagnostics.
- Benchmark new/crossing/cancel operations; store baseline results.
- Exit: deterministic example and replay reproducibly produce identical digests.

### Phase 2A: Trading agents and scenario processes (Days 8-10)

- Implement baseline market maker, inventory-aware maker, value, momentum, and noise agent policies.
- Add delivered observations, deterministic agent RNG, order/report/market-data delays.
- Add synthetic fundamental model and two shock scenarios, plus event volume caps.
- Exit: one-hour simulated market with meaningful trades, two-sided quotes, and reproducible agent activity.

### Phase 2B: Accounting and experiments (Days 11-14)

- Add trade ledger, positions, P&L, fee hooks, exposure and drawdown metrics.
- Export manifest, canonical events, trades, snapshots, positions, and summaries.
- Add paired market-maker comparisons over configurable seeded batches in Python/CLI.
- Exit: >=200 paired-seed comparison runs (if hardware permits) with uncertainty reporting and coherent balance reconciliation.

### Phase 3A: Service integration (Days 15-17)

- Add pybind11 run-level wrapper, Python schemas, FastAPI endpoints, worker orchestration.
- Keep run metadata in a local SQLite registry and run jobs on an in-process worker;
  PostgreSQL and a Redis queue are deferred (ADR 0005). Preserve the CLI.
- Build the research terminal: scenario and experiment configuration, progress, synchronized
  run charts and depth, the execution stream, agent accounting, and paired comparisons.
- Add the replay timeline, verification status and diagnostics, and first-divergence
  inspection.
- Exit: browser can initiate a bounded scenario, view results, and inspect artifacts.

### Phase 3B: AI and polish (Days 18-21)

- Add typed research tools, AI experiment planner, report grounding and evaluation.
- Run end-to-end regression, load/memory checks, threat tests, and documentation/examples.
- Exit: a research query launches validated experiments and produces a traceable report; reference demos reproduce from clean build.

### Phase 4A: Observed historical equity data

- Add provider-independent Python acquisition with an offline fabricated fixture and an
  authorized Alpaca adapter. Credentials remain backend-only and a real import requires
  an explicit entitlement/cost gate.
- Retain immutable raw bytes, then normalize trades and L1 quotes in C++ using exact
  micro-dollar prices, UTC nanoseconds with source precision, and deterministic
  source-aware ordering.
- Publish a separate historical artifact family, native observation playback, data-quality
  report, observed-market metrics, SHA-256 verification, bounded FastAPI reads, and a
  Historical terminal workspace.
- Keep provider quotes out of the synthetic order book and provider prints out of agent
  accounting. Comparison with synthetic runs is diagnostic and definition-aware only.
- L2/L3 reconstruction, paper execution, hypothetical fills, strategy P&L on historical
  records, live feeds, and brokerage access remain out of scope.

[ADR 0007](docs/adr/0007-observed-historical-market-data.md) fixes the schema, ordering,
artifact, provider, licensing, and compatibility decisions.

### Phase 4B: Live market data

- Hold one upstream Alpaca stock stream (IEX, the provider's test stream, or a loopback
  fabricated mock) in an explicit gateway state machine with bounded reconnection, a
  per-user connection lock, and a symbol ceiling. Credentials remain backend-only.
- Normalize live trades and L1 quotes in C++ with the Phase 4A price and timestamp
  conversions, one native call per batch, keeping source, receipt, and processing clocks
  separate and classifying out-of-order and duplicate records instead of reordering them.
- Fan out to browsers over a versioned WebSocket protocol with bounded queues; every loss
  is counted and announced, followed by a snapshot.
- Record provider frames verbatim with lifecycle events and finalize each capture into
  ordinary verified Phase 4A sessions.
- Order entry, paper execution, hypothetical fills, SIP, and L2 remain out of scope.

[ADR 0008](docs/adr/0008-live-market-data.md) fixes the gateway, clock, ordering, protocol,
recording, and access-control decisions.

### Later stretch work

- Optional C++ optimized order book variants and profiled allocation improvements.
- Multiple synthetic symbols with explicit event-order semantics.
- More realistic latency/agent calibration after a separately approved methodology.
- Advanced visualization, hosted deployments, and user access control.

## 21. Agent collaboration and execution protocol

### 21.1 First-read instructions to any coding agent

```text
You are implementing MarketTwin. Read architecture.md, the current phase, and related tests before modifying code. C++20 is the only authoritative matching/simulation runtime. Preserve deterministic logical-time order, integer price ticks, explicit RNG streams, invariant checks, and replay compatibility. Do not introduce real trading, per-event Python callbacks, multi-threaded matching, or premature microservices. Work only in assigned files, create tests before integration, and finish with a concise implementation report (files changed, test commands/results, assumptions, unresolved questions). If a design change is needed, propose an ADR before changing contracts.
```

### 21.2 Parallel work stream suggestions

- Agent A: pure exchange core and invariant tests.
- Agent B: scheduler/events and deterministic replay, consuming agreed exchange interface.
- Agent C: numeric types, accounting, metrics, and schema fixtures after matching schemas settle.
- Agent D: C++ trading-agent strategies and scenario configs after kernel observation contract settles.
- Agent E: research scripts and statistical comparison after artifact schema settles.
- Agent F: API/UI after summary and persisted-snapshot schemas settle.
- Agent G: AI tooling/evaluations after API contract and experiment schema settle.
- Integration owner handles cross-cutting merges, ADR decisions, CI, and golden fixtures.

### 21.3 Agent branch/merge discipline

- Each agent works on a branch or isolated worktree with non-overlapping file ownership.
- Interfaces and schemas are frozen for the sprint once dependent implementations start.
- No agent should “fix” another subsystem by changing semantics without a coordinated review.
- Commit or patch includes test names and commands; generated data and secrets remain untracked.
- Integration merges one feature slice at a time and executes full native and replay test suites.

## 22. Core acceptance test examples

- Buy limit crossing multiple price levels executes cheapest ask first.
- Sell limit crossing multiple price levels executes highest bid first.
- Orders at identical price fill oldest first.
- Partial fills retain original resting priority for remainder.
- Market residual never becomes a resting order.
- IOC residual never becomes a resting order.
- Canceled order cannot later execute unless a distinct new order is submitted.
- Unknown order cancellation returns explicit not-found status.
- Cross-agent cancel without ownership fails.
- Duplicate order IDs are rejected without corruption.
- Equal-timestamp commands resolve by assigned sequence.
- Empty book yields optional best quote and invalid midpoint flag.
- No zero-quantity resting records persist.
- Mixed submit/cancel sequences preserve order/index consistency.
- Reported matched shares equal inventory transfers exactly.
- A cancellation arriving after fill reports already-filled/not-found, not phantom cancel.
- Replaying a run yields identical normalized trade sequence and final book hash.
- Deliberately corrupting one trace quantity produces first-divergence diagnostic.
- Same seed/config/build produces same trace digest on documented primary platform.
- Different seeds produce meaningful stochastic variation without violating invariants.
- Python wrapper returns same summary as native CLI for identical config.
- UI shows `N/A` if missing best bid or ask.
- AI rejects unsupported strategy names and oversized experiment counts.

## 23. Risk register

| Risk | Symptom | Mitigation | Owner |
|---|---|---|---|
| Book corruption | Stale iterators / phantom fills | Invariant tests, sanitizers, randomized traces | Exchange |
| Nondeterminism | Different hashes on replay | Central sequence IDs, isolated RNG streams, stable serialization | Kernel |
| Arithmetic errors | P&L drift / overflow | Integer ticks, checked intermediates, ledger reconciliation | Accounting |
| Unrealistic simulation | Agent strategy wins only in toy corner case | Parameter sweeps, sensitivity tests, disclosure | Research |
| Agent look-ahead | Suspiciously perfect signals | Delivered observation boundary and trace audits | Agents |
| Runaway compute | Infinite event loop / disk bloat | Event caps, budgets, periodic checkpoints | Kernel / Worker |
| Misleading metrics | Overstated profitability | Fees, tails, paired experiments, provenance | Analytics |
| AI hallucinations | Unverified numbers in reports | Structured tool grounding and evaluation | AI |
| Interface drift | C++/Python schema mismatch | Versioned schemas and contract tests | Integration |
| Scope creep | Infrastructure built before matching works | Phase gates and non-goals | Project lead |

## 24. Architectural decision records to create

- ADR-0001: C++20 single-threaded deterministic kernel.
- ADR-0002: Tick/quantity/money units and overflow behavior.
- ADR-0003: C++ order-book containers and iterator lifetime.
- ADR-0004: Self-trade prevention and cancellation outcomes.
- ADR-0005: Canonical event log, hash format, and replay modes.
- ADR-0006: RNG algorithm, stream partition, and distribution portability.
- ADR-0007: Accounting convention and mark-to-market fallbacks.
- ADR-0008: pybind11 vs subprocess CLI for integration and why. (Settled in `docs/adr/0005`.)
- ADR-0009: Worker queue and result store selection. (Settled in `docs/adr/0005`.)
- ADR-0010: AI allowed tools, permission model, and execution budgets.

## 25. Launch checklist and definition of done

- [ ] Clean clone builds C++ core on supported toolchains.
- [ ] Exchange invariants, partial fills, cancel, and FIFO suites pass.
- [ ] Sanitizers pass representative randomized order streams.
- [ ] Kernel schedules ties deterministically and enforces caps.
- [ ] Trading agents have observation and latency boundaries.
- [ ] Ledger reconciles cash/shares and stable P&L definitions.
- [ ] Replay verifies traces and highlights intentional corruption.
- [ ] Paired experiment outputs sample size, effect, uncertainty and config provenance.
- [ ] CLI and Python binding agree on run summary.
- [ ] API and dashboard produce a complete reproducible experiment.
- [ ] AI research tools cannot execute arbitrary code or real trades.
- [ ] User-facing charts display units, validity status, and fidelity warning.
- [ ] README includes one-command demo and non-goals.
- [ ] Showcase includes an example liquidity shock and maker comparison.

---

# Appendix A. Exhaustive implementation backlog and verification catalog

The following catalog is intentionally explicit so coding agents have small, independently verifiable work items. Each item specifies an implementation rule and a test/observation expectation. It is a task checklist, not permission to implement future phases early.
## A.01 Order validation verification matrix
All cases in this matrix are expected to be covered before declaring the `order validation` component production-quality for the simulator. Most are automatable; UI cases can be component or integration tests.
### A.01.01 New Limit Buy
- **Setup:** Create the smallest deterministic fixture exercising `new limit buy` in the order validation subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.01.02 New Limit Sell
- **Setup:** Create the smallest deterministic fixture exercising `new limit sell` in the order validation subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.01.03 Market Buy
- **Setup:** Create the smallest deterministic fixture exercising `market buy` in the order validation subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.01.04 Market Sell
- **Setup:** Create the smallest deterministic fixture exercising `market sell` in the order validation subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.01.05 Ioc Limit
- **Setup:** Create the smallest deterministic fixture exercising `IOC limit` in the order validation subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.01.06 Gtc Limit
- **Setup:** Create the smallest deterministic fixture exercising `GTC limit` in the order validation subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.01.07 Duplicate Order Id
- **Setup:** Create the smallest deterministic fixture exercising `duplicate order ID` in the order validation subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.01.08 Unknown Symbol
- **Setup:** Create the smallest deterministic fixture exercising `unknown symbol` in the order validation subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.01.09 Invalid Agent Owner
- **Setup:** Create the smallest deterministic fixture exercising `invalid agent owner` in the order validation subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.01.10 Zero Quantity
- **Setup:** Create the smallest deterministic fixture exercising `zero quantity` in the order validation subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.01.11 Zero Price Ticks
- **Setup:** Create the smallest deterministic fixture exercising `zero price ticks` in the order validation subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.01.12 Overflow Quantity
- **Setup:** Create the smallest deterministic fixture exercising `overflow quantity` in the order validation subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
## A.02 Order matching verification matrix
All cases in this matrix are expected to be covered before declaring the `order matching` component production-quality for the simulator. Most are automatable; UI cases can be component or integration tests.
### A.02.01 Best Bid Priority
- **Setup:** Create the smallest deterministic fixture exercising `best bid priority` in the order matching subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.02.02 Best Ask Priority
- **Setup:** Create the smallest deterministic fixture exercising `best ask priority` in the order matching subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.02.03 Multiple Price Levels
- **Setup:** Create the smallest deterministic fixture exercising `multiple price levels` in the order matching subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.02.04 Same-Price Fifo
- **Setup:** Create the smallest deterministic fixture exercising `same-price FIFO` in the order matching subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.02.05 Partial Resting Fill
- **Setup:** Create the smallest deterministic fixture exercising `partial resting fill` in the order matching subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.02.06 Partial Aggressor Fill
- **Setup:** Create the smallest deterministic fixture exercising `partial aggressor fill` in the order matching subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.02.07 Full-Book Sweep
- **Setup:** Create the smallest deterministic fixture exercising `full-book sweep` in the order matching subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.02.08 Empty Opposite Side
- **Setup:** Create the smallest deterministic fixture exercising `empty opposite side` in the order matching subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.02.09 Uncrossed Rest
- **Setup:** Create the smallest deterministic fixture exercising `uncrossed rest` in the order matching subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.02.10 Crossing Ioc
- **Setup:** Create the smallest deterministic fixture exercising `crossing IOC` in the order matching subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.02.11 Market No-Liquidity
- **Setup:** Create the smallest deterministic fixture exercising `market no-liquidity` in the order matching subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.02.12 Trade Id Sequence
- **Setup:** Create the smallest deterministic fixture exercising `trade ID sequence` in the order matching subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
## A.03 Cancellation verification matrix
All cases in this matrix are expected to be covered before declaring the `cancellation` component production-quality for the simulator. Most are automatable; UI cases can be component or integration tests.
### A.03.01 Cancel Newest
- **Setup:** Create the smallest deterministic fixture exercising `cancel newest` in the cancellation subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.03.02 Cancel Oldest
- **Setup:** Create the smallest deterministic fixture exercising `cancel oldest` in the cancellation subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.03.03 Cancel Sole Level Order
- **Setup:** Create the smallest deterministic fixture exercising `cancel sole level order` in the cancellation subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.03.04 Cancel Mid-Queue
- **Setup:** Create the smallest deterministic fixture exercising `cancel mid-queue` in the cancellation subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.03.05 Unknown Id
- **Setup:** Create the smallest deterministic fixture exercising `unknown ID` in the cancellation subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.03.06 Wrong Owner
- **Setup:** Create the smallest deterministic fixture exercising `wrong owner` in the cancellation subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.03.07 Repeated Cancel
- **Setup:** Create the smallest deterministic fixture exercising `repeated cancel` in the cancellation subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.03.08 Cancel After Full Fill
- **Setup:** Create the smallest deterministic fixture exercising `cancel after full fill` in the cancellation subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.03.09 Cancel After Partial Fill
- **Setup:** Create the smallest deterministic fixture exercising `cancel after partial fill` in the cancellation subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.03.10 Cancel Before Arrival
- **Setup:** Create the smallest deterministic fixture exercising `cancel before arrival` in the cancellation subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.03.11 Cancel After Execution
- **Setup:** Create the smallest deterministic fixture exercising `cancel after execution` in the cancellation subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.03.12 Same-Time Cancel Race
- **Setup:** Create the smallest deterministic fixture exercising `same-time cancel race` in the cancellation subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
## A.04 Book snapshots verification matrix
All cases in this matrix are expected to be covered before declaring the `book snapshots` component production-quality for the simulator. Most are automatable; UI cases can be component or integration tests.
### A.04.01 One Level
- **Setup:** Create the smallest deterministic fixture exercising `one level` in the book snapshots subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.04.02 Multiple Levels
- **Setup:** Create the smallest deterministic fixture exercising `multiple levels` in the book snapshots subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.04.03 Depth Zero Rejection
- **Setup:** Create the smallest deterministic fixture exercising `depth zero rejection` in the book snapshots subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.04.04 Depth Cap
- **Setup:** Create the smallest deterministic fixture exercising `depth cap` in the book snapshots subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.04.05 Missing Bid
- **Setup:** Create the smallest deterministic fixture exercising `missing bid` in the book snapshots subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.04.06 Missing Ask
- **Setup:** Create the smallest deterministic fixture exercising `missing ask` in the book snapshots subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.04.07 Both Sides Empty
- **Setup:** Create the smallest deterministic fixture exercising `both sides empty` in the book snapshots subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.04.08 Aggregated Size
- **Setup:** Create the smallest deterministic fixture exercising `aggregated size` in the book snapshots subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.04.09 Top-Of-Book Changes
- **Setup:** Create the smallest deterministic fixture exercising `top-of-book changes` in the book snapshots subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.04.10 Post-Cancel Depth
- **Setup:** Create the smallest deterministic fixture exercising `post-cancel depth` in the book snapshots subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.04.11 Post-Partial-Fill Depth
- **Setup:** Create the smallest deterministic fixture exercising `post-partial-fill depth` in the book snapshots subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.04.12 Stable Ordering
- **Setup:** Create the smallest deterministic fixture exercising `stable ordering` in the book snapshots subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
## A.05 Scheduler verification matrix
All cases in this matrix are expected to be covered before declaring the `scheduler` component production-quality for the simulator. Most are automatable; UI cases can be component or integration tests.
### A.05.01 Empty Queue
- **Setup:** Create the smallest deterministic fixture exercising `empty queue` in the scheduler subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.05.02 Tie Timestamps
- **Setup:** Create the smallest deterministic fixture exercising `tie timestamps` in the scheduler subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.05.03 Zero-Latency Schedule
- **Setup:** Create the smallest deterministic fixture exercising `zero-latency schedule` in the scheduler subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.05.04 Future Schedule
- **Setup:** Create the smallest deterministic fixture exercising `future schedule` in the scheduler subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.05.05 Past-Event Rejection
- **Setup:** Create the smallest deterministic fixture exercising `past-event rejection` in the scheduler subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.05.06 Same-Time Insertion
- **Setup:** Create the smallest deterministic fixture exercising `same-time insertion` in the scheduler subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.05.07 Event Id Monotonicity
- **Setup:** Create the smallest deterministic fixture exercising `event ID monotonicity` in the scheduler subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.05.08 Stop Event
- **Setup:** Create the smallest deterministic fixture exercising `stop event` in the scheduler subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.05.09 Event-Count Cap
- **Setup:** Create the smallest deterministic fixture exercising `event-count cap` in the scheduler subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.05.10 Max Simulated Time
- **Setup:** Create the smallest deterministic fixture exercising `max simulated time` in the scheduler subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.05.11 Malformed Payload
- **Setup:** Create the smallest deterministic fixture exercising `malformed payload` in the scheduler subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.05.12 Queue Capacity Limit
- **Setup:** Create the smallest deterministic fixture exercising `queue capacity limit` in the scheduler subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
## A.06 Market data verification matrix
All cases in this matrix are expected to be covered before declaring the `market data` component production-quality for the simulator. Most are automatable; UI cases can be component or integration tests.
### A.06.01 Quote Update
- **Setup:** Create the smallest deterministic fixture exercising `quote update` in the market data subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.06.02 Trade Print
- **Setup:** Create the smallest deterministic fixture exercising `trade print` in the market data subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.06.03 Snapshot Sampling
- **Setup:** Create the smallest deterministic fixture exercising `snapshot sampling` in the market data subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.06.04 Latency Propagation
- **Setup:** Create the smallest deterministic fixture exercising `latency propagation` in the market data subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.06.05 Late Message
- **Setup:** Create the smallest deterministic fixture exercising `late message` in the market data subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.06.06 Agent Local Clock
- **Setup:** Create the smallest deterministic fixture exercising `agent local clock` in the market data subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.06.07 No Future Lookahead
- **Setup:** Create the smallest deterministic fixture exercising `no future lookahead` in the market data subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.06.08 Missing Midpoint
- **Setup:** Create the smallest deterministic fixture exercising `missing midpoint` in the market data subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.06.09 Stale Observations
- **Setup:** Create the smallest deterministic fixture exercising `stale observations` in the market data subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.06.10 Report Ordering
- **Setup:** Create the smallest deterministic fixture exercising `report ordering` in the market data subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.06.11 Depth Truncation
- **Setup:** Create the smallest deterministic fixture exercising `depth truncation` in the market data subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.06.12 Shock Message Timing
- **Setup:** Create the smallest deterministic fixture exercising `shock message timing` in the market data subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
## A.07 RNG verification matrix
All cases in this matrix are expected to be covered before declaring the `rng` component production-quality for the simulator. Most are automatable; UI cases can be component or integration tests.
### A.07.01 Seed Initialization
- **Setup:** Create the smallest deterministic fixture exercising `seed initialization` in the rng subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.07.02 Agent Stream Split
- **Setup:** Create the smallest deterministic fixture exercising `agent stream split` in the rng subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.07.03 Scenario Stream Split
- **Setup:** Create the smallest deterministic fixture exercising `scenario stream split` in the rng subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.07.04 Repeatable Draws
- **Setup:** Create the smallest deterministic fixture exercising `repeatable draws` in the rng subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.07.05 Stream Independence
- **Setup:** Create the smallest deterministic fixture exercising `stream independence` in the rng subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.07.06 Distribution Bounds
- **Setup:** Create the smallest deterministic fixture exercising `distribution bounds` in the rng subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.07.07 Large Seed Values
- **Setup:** Create the smallest deterministic fixture exercising `large seed values` in the rng subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.07.08 Zero Seed
- **Setup:** Create the smallest deterministic fixture exercising `zero seed` in the rng subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.07.09 Versioned Algorithm
- **Setup:** Create the smallest deterministic fixture exercising `versioned algorithm` in the rng subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.07.10 Same Seed Replay
- **Setup:** Create the smallest deterministic fixture exercising `same seed replay` in the rng subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.07.11 Different Seed Diversity
- **Setup:** Create the smallest deterministic fixture exercising `different seed diversity` in the rng subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.07.12 Random-State Digest
- **Setup:** Create the smallest deterministic fixture exercising `random-state digest` in the rng subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
## A.08 Market maker verification matrix
All cases in this matrix are expected to be covered before declaring the `market maker` component production-quality for the simulator. Most are automatable; UI cases can be component or integration tests.
### A.08.01 Quote Initialization
- **Setup:** Create the smallest deterministic fixture exercising `quote initialization` in the market maker subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.08.02 Quote Replacement
- **Setup:** Create the smallest deterministic fixture exercising `quote replacement` in the market maker subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.08.03 Bid Size Clamp
- **Setup:** Create the smallest deterministic fixture exercising `bid size clamp` in the market maker subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.08.04 Ask Size Clamp
- **Setup:** Create the smallest deterministic fixture exercising `ask size clamp` in the market maker subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.08.05 Inventory Limit
- **Setup:** Create the smallest deterministic fixture exercising `inventory limit` in the market maker subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.08.06 Cash Constraint
- **Setup:** Create the smallest deterministic fixture exercising `cash constraint` in the market maker subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.08.07 Stale Quote Cancel
- **Setup:** Create the smallest deterministic fixture exercising `stale quote cancel` in the market maker subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.08.08 Market Shock Reaction
- **Setup:** Create the smallest deterministic fixture exercising `market shock reaction` in the market maker subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.08.09 Lack Of Midpoint
- **Setup:** Create the smallest deterministic fixture exercising `lack of midpoint` in the market maker subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.08.10 Spread Floor
- **Setup:** Create the smallest deterministic fixture exercising `spread floor` in the market maker subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.08.11 Risk Cap
- **Setup:** Create the smallest deterministic fixture exercising `risk cap` in the market maker subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.08.12 Shutdown Cleanup
- **Setup:** Create the smallest deterministic fixture exercising `shutdown cleanup` in the market maker subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
## A.09 Inventory maker verification matrix
All cases in this matrix are expected to be covered before declaring the `inventory maker` component production-quality for the simulator. Most are automatable; UI cases can be component or integration tests.
### A.09.01 Positive Inventory Skew
- **Setup:** Create the smallest deterministic fixture exercising `positive inventory skew` in the inventory maker subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.09.02 Negative Inventory Skew
- **Setup:** Create the smallest deterministic fixture exercising `negative inventory skew` in the inventory maker subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.09.03 Risk Aversion Zero
- **Setup:** Create the smallest deterministic fixture exercising `risk aversion zero` in the inventory maker subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.09.04 High Volatility
- **Setup:** Create the smallest deterministic fixture exercising `high volatility` in the inventory maker subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.09.05 Low Volatility
- **Setup:** Create the smallest deterministic fixture exercising `low volatility` in the inventory maker subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.09.06 Near Horizon
- **Setup:** Create the smallest deterministic fixture exercising `near horizon` in the inventory maker subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.09.07 No Volatility Estimate
- **Setup:** Create the smallest deterministic fixture exercising `no volatility estimate` in the inventory maker subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.09.08 Quote Level Rounding
- **Setup:** Create the smallest deterministic fixture exercising `quote level rounding` in the inventory maker subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.09.09 Position Cap
- **Setup:** Create the smallest deterministic fixture exercising `position cap` in the inventory maker subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.09.10 Paired Baseline
- **Setup:** Create the smallest deterministic fixture exercising `paired baseline` in the inventory maker subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.09.11 Severe Liquidity Loss
- **Setup:** Create the smallest deterministic fixture exercising `severe liquidity loss` in the inventory maker subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.09.12 Loss Tail
- **Setup:** Create the smallest deterministic fixture exercising `loss tail` in the inventory maker subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
## A.10 Momentum agent verification matrix
All cases in this matrix are expected to be covered before declaring the `momentum agent` component production-quality for the simulator. Most are automatable; UI cases can be component or integration tests.
### A.10.01 Warmup
- **Setup:** Create the smallest deterministic fixture exercising `warmup` in the momentum agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.10.02 Positive Signal
- **Setup:** Create the smallest deterministic fixture exercising `positive signal` in the momentum agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.10.03 Negative Signal
- **Setup:** Create the smallest deterministic fixture exercising `negative signal` in the momentum agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.10.04 Neutral Signal
- **Setup:** Create the smallest deterministic fixture exercising `neutral signal` in the momentum agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.10.05 Lookback Lag
- **Setup:** Create the smallest deterministic fixture exercising `lookback lag` in the momentum agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.10.06 Missing Prices
- **Setup:** Create the smallest deterministic fixture exercising `missing prices` in the momentum agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.10.07 Cooldown
- **Setup:** Create the smallest deterministic fixture exercising `cooldown` in the momentum agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.10.08 Position Cap
- **Setup:** Create the smallest deterministic fixture exercising `position cap` in the momentum agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.10.09 Max Message Rate
- **Setup:** Create the smallest deterministic fixture exercising `max message rate` in the momentum agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.10.10 Latency Effect
- **Setup:** Create the smallest deterministic fixture exercising `latency effect` in the momentum agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.10.11 Signal Decay
- **Setup:** Create the smallest deterministic fixture exercising `signal decay` in the momentum agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.10.12 Shock Reversal
- **Setup:** Create the smallest deterministic fixture exercising `shock reversal` in the momentum agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
## A.11 Value agent verification matrix
All cases in this matrix are expected to be covered before declaring the `value agent` component production-quality for the simulator. Most are automatable; UI cases can be component or integration tests.
### A.11.01 Fundamental Estimate
- **Setup:** Create the smallest deterministic fixture exercising `fundamental estimate` in the value agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.11.02 Valuation Gap Threshold
- **Setup:** Create the smallest deterministic fixture exercising `valuation gap threshold` in the value agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.11.03 Measurement Noise
- **Setup:** Create the smallest deterministic fixture exercising `measurement noise` in the value agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.11.04 Delayed Fundamental
- **Setup:** Create the smallest deterministic fixture exercising `delayed fundamental` in the value agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.11.05 Buying Discount
- **Setup:** Create the smallest deterministic fixture exercising `buying discount` in the value agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.11.06 Selling Premium
- **Setup:** Create the smallest deterministic fixture exercising `selling premium` in the value agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.11.07 Capital Limit
- **Setup:** Create the smallest deterministic fixture exercising `capital limit` in the value agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.11.08 Position Limit
- **Setup:** Create the smallest deterministic fixture exercising `position limit` in the value agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.11.09 Price Jump
- **Setup:** Create the smallest deterministic fixture exercising `price jump` in the value agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.11.10 Threshold Hysteresis
- **Setup:** Create the smallest deterministic fixture exercising `threshold hysteresis` in the value agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.11.11 No Midpoint
- **Setup:** Create the smallest deterministic fixture exercising `no midpoint` in the value agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.11.12 Shock Recovery
- **Setup:** Create the smallest deterministic fixture exercising `shock recovery` in the value agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
## A.12 Noise agent verification matrix
All cases in this matrix are expected to be covered before declaring the `noise agent` component production-quality for the simulator. Most are automatable; UI cases can be component or integration tests.
### A.12.01 Buy Sell Balance
- **Setup:** Create the smallest deterministic fixture exercising `buy sell balance` in the noise agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.12.02 Size Distribution
- **Setup:** Create the smallest deterministic fixture exercising `size distribution` in the noise agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.12.03 Arrival Distribution
- **Setup:** Create the smallest deterministic fixture exercising `arrival distribution` in the noise agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.12.04 Limit Offsets
- **Setup:** Create the smallest deterministic fixture exercising `limit offsets` in the noise agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.12.05 Market Order Probability
- **Setup:** Create the smallest deterministic fixture exercising `market order probability` in the noise agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.12.06 Per-Agent Rng
- **Setup:** Create the smallest deterministic fixture exercising `per-agent RNG` in the noise agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.12.07 Zero Depth
- **Setup:** Create the smallest deterministic fixture exercising `zero depth` in the noise agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.12.08 Extreme Imbalance
- **Setup:** Create the smallest deterministic fixture exercising `extreme imbalance` in the noise agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.12.09 High Arrival Rate
- **Setup:** Create the smallest deterministic fixture exercising `high arrival rate` in the noise agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.12.10 Burst Cap
- **Setup:** Create the smallest deterministic fixture exercising `burst cap` in the noise agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.12.11 Truncated Sizes
- **Setup:** Create the smallest deterministic fixture exercising `truncated sizes` in the noise agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.12.12 Replay Equivalence
- **Setup:** Create the smallest deterministic fixture exercising `replay equivalence` in the noise agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
## A.13 Accounting verification matrix
All cases in this matrix are expected to be covered before declaring the `accounting` component production-quality for the simulator. Most are automatable; UI cases can be component or integration tests.
### A.13.01 Buyer Cash Debit
- **Setup:** Create the smallest deterministic fixture exercising `buyer cash debit` in the accounting subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.13.02 Seller Cash Credit
- **Setup:** Create the smallest deterministic fixture exercising `seller cash credit` in the accounting subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.13.03 Buyer Shares Credit
- **Setup:** Create the smallest deterministic fixture exercising `buyer shares credit` in the accounting subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.13.04 Seller Shares Debit
- **Setup:** Create the smallest deterministic fixture exercising `seller shares debit` in the accounting subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.13.05 Fee Application
- **Setup:** Create the smallest deterministic fixture exercising `fee application` in the accounting subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.13.06 Partial Fill Ledger
- **Setup:** Create the smallest deterministic fixture exercising `partial fill ledger` in the accounting subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.13.07 Short-Position Policy
- **Setup:** Create the smallest deterministic fixture exercising `short-position policy` in the accounting subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.13.08 Insufficient Cash
- **Setup:** Create the smallest deterministic fixture exercising `insufficient cash` in the accounting subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.13.09 Mark At Midpoint
- **Setup:** Create the smallest deterministic fixture exercising `mark at midpoint` in the accounting subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.13.10 Fallback Mark
- **Setup:** Create the smallest deterministic fixture exercising `fallback mark` in the accounting subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.13.11 Realized P&L
- **Setup:** Create the smallest deterministic fixture exercising `realized P&L` in the accounting subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.13.12 Equity Conservation
- **Setup:** Create the smallest deterministic fixture exercising `equity conservation` in the accounting subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
## A.14 Metrics verification matrix
All cases in this matrix are expected to be covered before declaring the `metrics` component production-quality for the simulator. Most are automatable; UI cases can be component or integration tests.
### A.14.01 Quoted Spread
- **Setup:** Create the smallest deterministic fixture exercising `quoted spread` in the metrics subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.14.02 Depth N
- **Setup:** Create the smallest deterministic fixture exercising `depth N` in the metrics subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.14.03 Volume
- **Setup:** Create the smallest deterministic fixture exercising `volume` in the metrics subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.14.04 Agent Participation
- **Setup:** Create the smallest deterministic fixture exercising `agent participation` in the metrics subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.14.05 Maker Realized P&L
- **Setup:** Create the smallest deterministic fixture exercising `maker realized P&L` in the metrics subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.14.06 Unrealized P&L
- **Setup:** Create the smallest deterministic fixture exercising `unrealized P&L` in the metrics subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.14.07 Drawdown
- **Setup:** Create the smallest deterministic fixture exercising `drawdown` in the metrics subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.14.08 Inventory Duration
- **Setup:** Create the smallest deterministic fixture exercising `inventory duration` in the metrics subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.14.09 Fill Rate
- **Setup:** Create the smallest deterministic fixture exercising `fill rate` in the metrics subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.14.10 Price Volatility
- **Setup:** Create the smallest deterministic fixture exercising `price volatility` in the metrics subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.14.11 Adverse Selection
- **Setup:** Create the smallest deterministic fixture exercising `adverse selection` in the metrics subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.14.12 Missing Samples
- **Setup:** Create the smallest deterministic fixture exercising `missing samples` in the metrics subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
## A.15 Shocks verification matrix
All cases in this matrix are expected to be covered before declaring the `shocks` component production-quality for the simulator. Most are automatable; UI cases can be component or integration tests.
### A.15.01 Fundamental Down
- **Setup:** Create the smallest deterministic fixture exercising `fundamental down` in the shocks subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.15.02 Fundamental Up
- **Setup:** Create the smallest deterministic fixture exercising `fundamental up` in the shocks subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.15.03 Liquidity Withdrawal
- **Setup:** Create the smallest deterministic fixture exercising `liquidity withdrawal` in the shocks subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.15.04 Liquidity Recovery
- **Setup:** Create the smallest deterministic fixture exercising `liquidity recovery` in the shocks subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.15.05 Sell Imbalance
- **Setup:** Create the smallest deterministic fixture exercising `sell imbalance` in the shocks subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.15.06 Buy Imbalance
- **Setup:** Create the smallest deterministic fixture exercising `buy imbalance` in the shocks subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.15.07 Volatility Surge
- **Setup:** Create the smallest deterministic fixture exercising `volatility surge` in the shocks subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.15.08 Latency Shock
- **Setup:** Create the smallest deterministic fixture exercising `latency shock` in the shocks subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.15.09 Asymmetric Shock
- **Setup:** Create the smallest deterministic fixture exercising `asymmetric shock` in the shocks subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.15.10 Overlapping Shocks
- **Setup:** Create the smallest deterministic fixture exercising `overlapping shocks` in the shocks subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.15.11 Shock Horizon
- **Setup:** Create the smallest deterministic fixture exercising `shock horizon` in the shocks subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.15.12 Shock Audit Trail
- **Setup:** Create the smallest deterministic fixture exercising `shock audit trail` in the shocks subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
## A.16 Event logging verification matrix
All cases in this matrix are expected to be covered before declaring the `event logging` component production-quality for the simulator. Most are automatable; UI cases can be component or integration tests.
### A.16.01 Canonical Order Input
- **Setup:** Create the smallest deterministic fixture exercising `canonical order input` in the event logging subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.16.02 Cancel Input
- **Setup:** Create the smallest deterministic fixture exercising `cancel input` in the event logging subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.16.03 Trade Output
- **Setup:** Create the smallest deterministic fixture exercising `trade output` in the event logging subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.16.04 Rejection Output
- **Setup:** Create the smallest deterministic fixture exercising `rejection output` in the event logging subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.16.05 Timestamps
- **Setup:** Create the smallest deterministic fixture exercising `timestamps` in the event logging subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.16.06 Sequence Numbers
- **Setup:** Create the smallest deterministic fixture exercising `sequence numbers` in the event logging subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.16.07 Agent Identity
- **Setup:** Create the smallest deterministic fixture exercising `agent identity` in the event logging subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.16.08 Units
- **Setup:** Create the smallest deterministic fixture exercising `units` in the event logging subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.16.09 Schema Header
- **Setup:** Create the smallest deterministic fixture exercising `schema header` in the event logging subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.16.10 Artifact Checksum
- **Setup:** Create the smallest deterministic fixture exercising `artifact checksum` in the event logging subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.16.11 Truncated File
- **Setup:** Create the smallest deterministic fixture exercising `truncated file` in the event logging subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.16.12 Append-Only Finalization
- **Setup:** Create the smallest deterministic fixture exercising `append-only finalization` in the event logging subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
## A.17 Replay verification matrix
All cases in this matrix are expected to be covered before declaring the `replay` component production-quality for the simulator. Most are automatable; UI cases can be component or integration tests.
### A.17.01 Empty Run
- **Setup:** Create the smallest deterministic fixture exercising `empty run` in the replay subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.17.02 Single Trade
- **Setup:** Create the smallest deterministic fixture exercising `single trade` in the replay subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.17.03 Multi-Level Trade
- **Setup:** Create the smallest deterministic fixture exercising `multi-level trade` in the replay subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.17.04 Cancel-Only Run
- **Setup:** Create the smallest deterministic fixture exercising `cancel-only run` in the replay subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.17.05 Full Agent Run
- **Setup:** Create the smallest deterministic fixture exercising `full agent run` in the replay subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.17.06 Shock Run
- **Setup:** Create the smallest deterministic fixture exercising `shock run` in the replay subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.17.07 First Divergent Order
- **Setup:** Create the smallest deterministic fixture exercising `first divergent order` in the replay subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.17.08 First Divergent Fill
- **Setup:** Create the smallest deterministic fixture exercising `first divergent fill` in the replay subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.17.09 Corrupted Checksum
- **Setup:** Create the smallest deterministic fixture exercising `corrupted checksum` in the replay subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.17.10 Schema Mismatch
- **Setup:** Create the smallest deterministic fixture exercising `schema mismatch` in the replay subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.17.11 Checkpoint Comparison
- **Setup:** Create the smallest deterministic fixture exercising `checkpoint comparison` in the replay subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.17.12 Final Digest
- **Setup:** Create the smallest deterministic fixture exercising `final digest` in the replay subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
## A.18 Binding verification matrix
All cases in this matrix are expected to be covered before declaring the `binding` component production-quality for the simulator. Most are automatable; UI cases can be component or integration tests.
### A.18.01 Config Transfer
- **Setup:** Create the smallest deterministic fixture exercising `config transfer` in the binding subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.18.02 Optional Fields
- **Setup:** Create the smallest deterministic fixture exercising `optional fields` in the binding subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.18.03 Invalid Enum
- **Setup:** Create the smallest deterministic fixture exercising `invalid enum` in the binding subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.18.04 Native Exception
- **Setup:** Create the smallest deterministic fixture exercising `native exception` in the binding subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.18.05 Release Gil
- **Setup:** Create the smallest deterministic fixture exercising `release GIL` in the binding subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.18.06 Summary Transfer
- **Setup:** Create the smallest deterministic fixture exercising `summary transfer` in the binding subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.18.07 Artifact Path
- **Setup:** Create the smallest deterministic fixture exercising `artifact path` in the binding subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.18.08 Version Mismatch
- **Setup:** Create the smallest deterministic fixture exercising `version mismatch` in the binding subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.18.09 Multiple Independent Runs
- **Setup:** Create the smallest deterministic fixture exercising `multiple independent runs` in the binding subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.18.10 No Per-Event Callback
- **Setup:** Create the smallest deterministic fixture exercising `no per-event callback` in the binding subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.18.11 Wheel Import
- **Setup:** Create the smallest deterministic fixture exercising `wheel import` in the binding subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.18.12 Cli Parity
- **Setup:** Create the smallest deterministic fixture exercising `CLI parity` in the binding subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
## A.19 Workers verification matrix
All cases in this matrix are expected to be covered before declaring the `workers` component production-quality for the simulator. Most are automatable; UI cases can be component or integration tests.
### A.19.01 Enqueue
- **Setup:** Create the smallest deterministic fixture exercising `enqueue` in the workers subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.19.02 Status Transition
- **Setup:** Create the smallest deterministic fixture exercising `status transition` in the workers subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.19.03 Retry Semantics
- **Setup:** Create the smallest deterministic fixture exercising `retry semantics` in the workers subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.19.04 Idempotency
- **Setup:** Create the smallest deterministic fixture exercising `idempotency` in the workers subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.19.05 Cancel Request
- **Setup:** Create the smallest deterministic fixture exercising `cancel request` in the workers subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.19.06 Worker Crash
- **Setup:** Create the smallest deterministic fixture exercising `worker crash` in the workers subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.19.07 Bounded Cpu
- **Setup:** Create the smallest deterministic fixture exercising `bounded CPU` in the workers subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.19.08 Bounded Disk
- **Setup:** Create the smallest deterministic fixture exercising `bounded disk` in the workers subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.19.09 Timeout
- **Setup:** Create the smallest deterministic fixture exercising `timeout` in the workers subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.19.10 Artifact Finalization
- **Setup:** Create the smallest deterministic fixture exercising `artifact finalization` in the workers subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.19.11 Queue Backlog
- **Setup:** Create the smallest deterministic fixture exercising `queue backlog` in the workers subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.19.12 Progress Sampling
- **Setup:** Create the smallest deterministic fixture exercising `progress sampling` in the workers subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
## A.20 API verification matrix
All cases in this matrix are expected to be covered before declaring the `api` component production-quality for the simulator. Most are automatable; UI cases can be component or integration tests.
### A.20.01 Create Experiment
- **Setup:** Create the smallest deterministic fixture exercising `create experiment` in the api subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.20.02 Validate Config
- **Setup:** Create the smallest deterministic fixture exercising `validate config` in the api subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.20.03 Retrieve Status
- **Setup:** Create the smallest deterministic fixture exercising `retrieve status` in the api subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.20.04 Get Summary
- **Setup:** Create the smallest deterministic fixture exercising `get summary` in the api subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.20.05 Get Trades Page
- **Setup:** Create the smallest deterministic fixture exercising `get trades page` in the api subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.20.06 Get Depth Snapshot
- **Setup:** Create the smallest deterministic fixture exercising `get depth snapshot` in the api subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.20.07 Get Positions
- **Setup:** Create the smallest deterministic fixture exercising `get positions` in the api subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.20.08 Get Comparison
- **Setup:** Create the smallest deterministic fixture exercising `get comparison` in the api subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.20.09 Verify Replay Job
- **Setup:** Create the smallest deterministic fixture exercising `verify replay job` in the api subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.20.10 Websocket Reconnect
- **Setup:** Create the smallest deterministic fixture exercising `WebSocket reconnect` in the api subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.20.11 Invalid Id
- **Setup:** Create the smallest deterministic fixture exercising `invalid ID` in the api subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.20.12 Oversized Request
- **Setup:** Create the smallest deterministic fixture exercising `oversized request` in the api subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
## A.21 Web UI verification matrix
All cases in this matrix are expected to be covered before declaring the `web ui` component production-quality for the simulator. Most are automatable; UI cases can be component or integration tests.
### A.21.01 Experiment Builder
- **Setup:** Create the smallest deterministic fixture exercising `experiment builder` in the web ui subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.21.02 Parameter Bounds
- **Setup:** Create the smallest deterministic fixture exercising `parameter bounds` in the web ui subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.21.03 Run Loading
- **Setup:** Create the smallest deterministic fixture exercising `run loading` in the web ui subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.21.04 Run Failure
- **Setup:** Create the smallest deterministic fixture exercising `run failure` in the web ui subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.21.05 Trade Tape
- **Setup:** Create the smallest deterministic fixture exercising `trade tape` in the web ui subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.21.06 Depth Chart
- **Setup:** Create the smallest deterministic fixture exercising `depth chart` in the web ui subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.21.07 Midpoint Gap
- **Setup:** Create the smallest deterministic fixture exercising `midpoint gap` in the web ui subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.21.08 Inventory Curve
- **Setup:** Create the smallest deterministic fixture exercising `inventory curve` in the web ui subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.21.09 P&L Curve
- **Setup:** Create the smallest deterministic fixture exercising `P&L curve` in the web ui subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.21.10 Paired Comparison
- **Setup:** Create the smallest deterministic fixture exercising `paired comparison` in the web ui subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.21.11 Replay Scrubber
- **Setup:** Create the smallest deterministic fixture exercising `replay scrubber` in the web ui subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.21.12 Evidence Links
- **Setup:** Create the smallest deterministic fixture exercising `evidence links` in the web ui subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
## A.22 AI agent verification matrix
All cases in this matrix are expected to be covered before declaring the `ai agent` component production-quality for the simulator. Most are automatable; UI cases can be component or integration tests.
### A.22.01 Schema Validation
- **Setup:** Create the smallest deterministic fixture exercising `schema validation` in the ai agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.22.02 Tool Allowlist
- **Setup:** Create the smallest deterministic fixture exercising `tool allowlist` in the ai agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.22.03 Tool Timeout
- **Setup:** Create the smallest deterministic fixture exercising `tool timeout` in the ai agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.22.04 Invalid Experiment
- **Setup:** Create the smallest deterministic fixture exercising `invalid experiment` in the ai agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.22.05 Excessive Run Count
- **Setup:** Create the smallest deterministic fixture exercising `excessive run count` in the ai agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.22.06 Missing Run
- **Setup:** Create the smallest deterministic fixture exercising `missing run` in the ai agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.22.07 Numerical Grounding
- **Setup:** Create the smallest deterministic fixture exercising `numerical grounding` in the ai agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.22.08 Confidence Interval Wording
- **Setup:** Create the smallest deterministic fixture exercising `confidence interval wording` in the ai agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.22.09 Scenario Caveat
- **Setup:** Create the smallest deterministic fixture exercising `scenario caveat` in the ai agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.22.10 Prompt Injection
- **Setup:** Create the smallest deterministic fixture exercising `prompt injection` in the ai agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.22.11 Provenance Citation
- **Setup:** Create the smallest deterministic fixture exercising `provenance citation` in the ai agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.22.12 Replay Explanation
- **Setup:** Create the smallest deterministic fixture exercising `replay explanation` in the ai agent subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
## A.23 CI and release verification matrix
All cases in this matrix are expected to be covered before declaring the `ci and release` component production-quality for the simulator. Most are automatable; UI cases can be component or integration tests.
### A.23.01 Clean Configure
- **Setup:** Create the smallest deterministic fixture exercising `clean configure` in the ci and release subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.23.02 Warnings
- **Setup:** Create the smallest deterministic fixture exercising `warnings` in the ci and release subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.23.03 Clang-Format
- **Setup:** Create the smallest deterministic fixture exercising `clang-format` in the ci and release subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.23.04 Clang-Tidy
- **Setup:** Create the smallest deterministic fixture exercising `clang-tidy` in the ci and release subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.23.05 Unit Tests
- **Setup:** Create the smallest deterministic fixture exercising `unit tests` in the ci and release subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.23.06 Property Tests
- **Setup:** Create the smallest deterministic fixture exercising `property tests` in the ci and release subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.23.07 Sanitizers
- **Setup:** Create the smallest deterministic fixture exercising `sanitizers` in the ci and release subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.23.08 Replay Golden
- **Setup:** Create the smallest deterministic fixture exercising `replay golden` in the ci and release subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.23.09 Native Binding Build
- **Setup:** Create the smallest deterministic fixture exercising `native binding build` in the ci and release subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.23.10 Python Tests
- **Setup:** Create the smallest deterministic fixture exercising `Python tests` in the ci and release subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.23.11 Typescript Checks
- **Setup:** Create the smallest deterministic fixture exercising `TypeScript checks` in the ci and release subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
### A.23.12 Artifact Reproducibility
- **Setup:** Create the smallest deterministic fixture exercising `artifact reproducibility` in the ci and release subsystem.
- **Action:** Execute the relevant public interface through its intended boundary, not by mutating private state.
- **Expected:** Observed behavior conforms to the core contracts and leaves state internally consistent.
- **Evidence:** Assert exact state, explicit error/status as applicable, and a stable trace or metrics fixture where meaningful.
- **Regression:** Cover at least one adjacent edge case and verify no replay/accounting invariant is violated.
## Appendix B. Event envelopes and causal provenance
### B.01 Canonical envelope field
- **Requirement:** Canonical envelope field: `schema_version` determines interpretation and migrations.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### B.02 Canonical envelope field
- **Requirement:** Canonical envelope field: `run_id` identifies the originating simulation.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### B.03 Canonical envelope field
- **Requirement:** Canonical envelope field: `event_sequence` defines strict ordering independent of timestamps.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### B.04 Canonical envelope field
- **Requirement:** Canonical envelope field: `timestamp_ns` represents logical time, never wall time.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### B.05 Canonical envelope field
- **Requirement:** Canonical envelope field: `event_kind` is an enumerated type with a defined payload schema.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### B.06 Canonical envelope field
- **Requirement:** Canonical envelope field: `actor_id` names the system/agent source when applicable.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### B.07 Canonical envelope field
- **Requirement:** Canonical envelope field: `causation_id` links a report to its input event.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### B.08 Canonical envelope field
- **Requirement:** Canonical envelope field: `correlation_id` links a request/order/report lifecycle.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### B.09 Canonical envelope field
- **Requirement:** Canonical envelope field: `payload` uses integer units and explicit enum spellings.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### B.10 Canonical envelope field
- **Requirement:** Canonical envelope field: `payload_hash` guards content integrity when configured.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### B.11 Canonical envelope field
- **Requirement:** Canonical envelope field: `state_digest_after` optionally records debugging checkpoints.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### B.12 Canonical envelope field
- **Requirement:** Canonical envelope field: `source_version` ties output to engine semantics.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
## Appendix C. Experiments and comparative interpretation
### C.01 Pre-register primary metric and null hypothesis before inspecting wi
- **Requirement:** Pre-register primary metric and null hypothesis before inspecting winner charts.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### C.02 Use identical exogenous scenario seed for both market-maker policies
- **Requirement:** Use identical exogenous scenario seed for both market-maker policies where practicable.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### C.03 Use separate RNG streams for policy-specific actions to avoid accide
- **Requirement:** Use separate RNG streams for policy-specific actions to avoid accidental coupling.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### C.04 Track count of discarded or failed runs with explicit exclusion rule
- **Requirement:** Track count of discarded or failed runs with explicit exclusion rules.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### C.05 Use paired difference distribution as the primary comparison statist
- **Requirement:** Use paired difference distribution as the primary comparison statistic.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### C.06 Display confidence intervals with method and sample count.
- **Requirement:** Display confidence intervals with method and sample count.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### C.07 Compute maximum drawdown from a clearly defined mark-to-market sampl
- **Requirement:** Compute maximum drawdown from a clearly defined mark-to-market sample cadence.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### C.08 Compute profits net of modeled fees and transaction costs.
- **Requirement:** Compute profits net of modeled fees and transaction costs.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### C.09 Show realized/unrealized attribution and ending inventory, not only 
- **Requirement:** Show realized/unrealized attribution and ending inventory, not only cash P&L.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### C.10 Report spread, fill rate, and turnover as liquidity/service metrics.
- **Requirement:** Report spread, fill rate, and turnover as liquidity/service metrics.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### C.11 Include regime-by-regime and tail-risk slices, not just overall aver
- **Requirement:** Include regime-by-regime and tail-risk slices, not just overall average.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### C.12 State whether results are stylized, calibrated, or empirically bench
- **Requirement:** State whether results are stylized, calibrated, or empirically benchmarked.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
## Appendix D. Observability and troubleshooting
### D.01 All emitted run errors include run ID, component, code, and causal e
- **Requirement:** All emitted run errors include run ID, component, code, and causal event sequence.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### D.02 Log wall-clock processing time separately from simulated logical tim
- **Requirement:** Log wall-clock processing time separately from simulated logical time.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### D.03 Maintain counters for new, cancel, reject, partial-fill, full-fill, 
- **Requirement:** Maintain counters for new, cancel, reject, partial-fill, full-fill, and expiration events.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### D.04 Track logical queue depth and maximum depth.
- **Requirement:** Track logical queue depth and maximum depth.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### D.05 Track number of resting orders and number of nonempty price levels.
- **Requirement:** Track number of resting orders and number of nonempty price levels.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### D.06 Track active agents and message counts by agent class.
- **Requirement:** Track active agents and message counts by agent class.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### D.07 Track peak resident memory and on-disk artifact bytes.
- **Requirement:** Track peak resident memory and on-disk artifact bytes.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### D.08 Record earliest replay divergence with before/after book comparison.
- **Requirement:** Record earliest replay divergence with before/after book comparison.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### D.09 Show whether a missing midpoint is due to one-sided or fully empty b
- **Requirement:** Show whether a missing midpoint is due to one-sided or fully empty book.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### D.10 Distinguish calculation failure from missing market data.
- **Requirement:** Distinguish calculation failure from missing market data.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### D.11 Truncate high-volume diagnostic logs while retaining authoritative e
- **Requirement:** Truncate high-volume diagnostic logs while retaining authoritative event artifacts.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### D.12 Capture compiler/build revision and linked dependencies in run manif
- **Requirement:** Capture compiler/build revision and linked dependencies in run manifest.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
## Appendix E. Implementation decisions not to silently improvise
### E.01 Policy for self-trade prevention and ownership of resting orders.
- **Requirement:** Policy for self-trade prevention and ownership of resting orders.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### E.02 Money-unit scaling and fee rounding for integer ledger.
- **Requirement:** Money-unit scaling and fee rounding for integer ledger.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### E.03 RNG algorithm and distribution sampler implementations.
- **Requirement:** RNG algorithm and distribution sampler implementations.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### E.04 Mark-to-market fallback and quality flag when one side of book is ab
- **Requirement:** Mark-to-market fallback and quality flag when one side of book is absent.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### E.05 Choice of average-cost versus FIFO realized P&L.
- **Requirement:** Choice of average-cost versus FIFO realized P&L.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### E.06 Definition of trade-volume weighted metrics and sample cadence.
- **Requirement:** Definition of trade-volume weighted metrics and sample cadence.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### E.07 Definition of order status transitions and late cancels.
- **Requirement:** Definition of order status transitions and late cancels.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### E.08 Snapshot interval and depth retention budget.
- **Requirement:** Snapshot interval and depth retention budget.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### E.09 Canonical event serialization and hash algorithm.
- **Requirement:** Canonical event serialization and hash algorithm.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### E.10 Artifact conversion layer from canonical trace to Parquet.
- **Requirement:** Artifact conversion layer from canonical trace to Parquet.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### E.11 Queue implementation and job retry/idempotence policy.
- **Requirement:** Queue implementation and job retry/idempotence policy.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### E.12 API authentication model when moving beyond single-user localhost.
- **Requirement:** API authentication model when moving beyond single-user localhost.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
## Appendix F. Coding-agent prompt pack
### F.01 Exchange task
- **Requirement:** Exchange task: implement minimal public order API first, then enforce price-time and lifecycle invariants.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### F.02 Kernel task
- **Requirement:** Kernel task: implement monotonic heap ordering and event caps with tests before any trading agents.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### F.03 Replay task
- **Requirement:** Replay task: distinguish command input from generated execution outputs; do not double apply trades.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### F.04 Agent task
- **Requirement:** Agent task: consume permitted observations and produce intents; no exchange reference in strategy objects.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### F.05 Accounting task
- **Requirement:** Accounting task: use checked integer arithmetic and reconcilable trade-ledger mutations.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### F.06 Scenario task
- **Requirement:** Scenario task: alter exogenous signals or agent responses, not transaction prices directly.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### F.07 Experiment task
- **Requirement:** Experiment task: pair strategy seeds and show uncertainty with raw per-run results.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### F.08 Binding task
- **Requirement:** Binding task: keep per-run native calls coarse and return artifact references.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### F.09 API task
- **Requirement:** API task: implement durable job status and bounded result pagination.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### F.10 UI task
- **Requirement:** UI task: honor missing-data states and expose data provenance.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### F.11 AI task
- **Requirement:** AI task: enforce typed allowlisted tools and ground every reported number.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
### F.12 Integration task
- **Requirement:** Integration task: rerun native replay fixtures after any cross-subsystem change.
- **Owner action:** Implement or verify this contract in the assigned subsystem while preserving upstream interfaces.
- **Verification:** Add a deterministic test, an explicit schema assertion, or a reproducible inspection command.
- **Audit artifact:** Record the relevant run ID/test name, source revision, and pass/fail status.
- **Change control:** Any alternate semantics must be documented in an ADR and updated in dependent tests.
## Appendix G. Native engine task recipes
### G.01 Create typed PriceTicks and Quantity wrappers
**Objective.** Create typed PriceTicks and Quantity wrappers using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### G.02 Create order-side and time-in-force enums
**Objective.** Create order-side and time-in-force enums using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### G.03 Add ID generation owned by kernel/exchange
**Objective.** Add ID generation owned by kernel/exchange using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### G.04 Implement price level list and lookup index
**Objective.** Implement price level list and lookup index using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### G.05 Implement submit crossing loop
**Objective.** Implement submit crossing loop using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### G.06 Implement cancel lifecycle
**Objective.** Implement cancel lifecycle using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### G.07 Emit execution reports
**Objective.** Emit execution reports using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### G.08 Build debug invariant validator
**Objective.** Build debug invariant validator using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### G.09 Add scheduler priority-queue abstraction
**Objective.** Add scheduler priority-queue abstraction using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### G.10 Add event dispatch registry
**Objective.** Add event dispatch registry using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### G.11 Configure per-agent RNG streams
**Objective.** Configure per-agent RNG streams using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### G.12 Add market-data delivery queue
**Objective.** Add market-data delivery queue using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### G.13 Add synthetic fundamental process
**Objective.** Add synthetic fundamental process using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### G.14 Add deterministic shock injection
**Objective.** Add deterministic shock injection using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### G.15 Implement trade accounting ledger
**Objective.** Implement trade accounting ledger using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### G.16 Add trace encoder and SHA-256 checksum
**Objective.** Add trace encoder and SHA-256 checksum using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### G.17 Implement exchange replay executor
**Objective.** Implement exchange replay executor using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### G.18 Build standalone CLI and artifact manifest
**Objective.** Build standalone CLI and artifact manifest using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
## Appendix H. Platform task recipes
### H.01 Expose native Simulation.run through pybind11
**Objective.** Expose native Simulation.run through pybind11 using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### H.02 Package wheel with scikit-build-core
**Objective.** Package wheel with scikit-build-core using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### H.03 Define Pydantic ExperimentSpec schemas
**Objective.** Define Pydantic ExperimentSpec schemas using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### H.04 Implement paired seeds and strategy matrices
**Objective.** Implement paired seeds and strategy matrices using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### H.05 Implement bootstrap/paired confidence interval
**Objective.** Implement bootstrap/paired confidence interval using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### H.06 Create run metadata database migration
**Objective.** Create run metadata database migration using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### H.07 Implement idempotent worker job claim
**Objective.** Implement idempotent worker job claim using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### H.08 Create asynchronous FastAPI create endpoint
**Objective.** Create asynchronous FastAPI create endpoint using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### H.09 Create status and comparison endpoints
**Objective.** Create status and comparison endpoints using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### H.10 Implement paginated trade trace reads
**Objective.** Implement paginated trade trace reads using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### H.11 Implement sampled chart data endpoints
**Objective.** Implement sampled chart data endpoints using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### H.12 Add WebSocket progress feed
**Objective.** Add WebSocket progress feed using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### H.13 Build experiment builder UI
**Objective.** Build experiment builder UI using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### H.14 Build run overview dashboard
**Objective.** Build run overview dashboard using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### H.15 Build replay timeline and divergence view
**Objective.** Build replay timeline and divergence view using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### H.16 Add allowlisted AI experiment tools
**Objective.** Add allowlisted AI experiment tools using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### H.17 Create AI numeric-grounding test suite
**Objective.** Create AI numeric-grounding test suite using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### H.18 Document end-to-end smoke demo
**Objective.** Document end-to-end smoke demo using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
## Appendix I. Hardening task recipes
### I.01 Enable ASan/UBSan on native integration fixtures
**Objective.** Enable ASan/UBSan on native integration fixtures using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### I.02 Property test arbitrary order streams
**Objective.** Property test arbitrary order streams using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### I.03 Fuzz malformed command traces
**Objective.** Fuzz malformed command traces using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### I.04 Verify hash stability on supported toolchain
**Objective.** Verify hash stability on supported toolchain using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### I.05 Test process interruption during artifact creation
**Objective.** Test process interruption during artifact creation using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### I.06 Enforce event count and simulation-time limits
**Objective.** Enforce event count and simulation-time limits using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### I.07 Enforce max resting book size
**Objective.** Enforce max resting book size using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### I.08 Enforce job count and resource limits
**Objective.** Enforce job count and resource limits using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### I.09 Test invalid scenario parameter ranges
**Objective.** Test invalid scenario parameter ranges using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### I.10 Test agent stale feed behavior
**Objective.** Test agent stale feed behavior using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### I.11 Test missing bid or ask UI state
**Objective.** Test missing bid or ask UI state using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### I.12 Test no-trade scenario metrics
**Objective.** Test no-trade scenario metrics using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### I.13 Check external API and AI input validation
**Objective.** Check external API and AI input validation using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### I.14 Ensure generated output excluded from version control
**Objective.** Ensure generated output excluded from version control using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### I.15 Inspect log output for secrets
**Objective.** Inspect log output for secrets using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### I.16 Measure worst-case burst memory and latency
**Objective.** Measure worst-case burst memory and latency using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### I.17 Benchmark cancel-heavy versus fill-heavy workloads
**Objective.** Benchmark cancel-heavy versus fill-heavy workloads using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
### I.18 Re-run full regression from clean build
**Objective.** Re-run full regression from clean build using the contracts in the main architecture and the minimal public interfaces needed by dependents.
**Inputs.** Versioned configuration, explicit types, and deterministic test fixtures with fixed seeds as relevant.
**Implementation.** Keep domain logic in the specified owner module, avoid hidden global state, and validate errors before mutation.
**Unit tests.** Include happy path, invalid input, and deterministic boundary case; assert exact state rather than incidental log text.
**Integration tests.** Demonstrate compatibility with at least one upstream/downstream interface and canonical serialization where applicable.
**Observability.** Emit stable statuses or manifest fields sufficient to troubleshoot without attaching a debugger.
**Definition of done.** Build succeeds, tests pass, docs/contracts are updated, and another coding agent can reproduce the result.
## Appendix J. Worked exchange scenarios
### J.01 FIFO example
Resting asks A(100 ticks, 60 shares) and B(100 ticks, 40 shares) arrive in that order. A new market buy of 75 executes 60 against A then 15 against B, leaving B with 25 at the same priority. Verify trade IDs and the absence of A from order ID index.

**Acceptance evidence**
- Minimal reproducible fixture with explicit config/seed and expected outcome.
- Deterministic assertions for state, status, and output artifact integrity.
- Documentation of which phase introduces the scenario and how to rerun it.
### J.02 Multi-level sweep
Resting asks are 100 ticks x 50 and 101 ticks x 80. Buy limit for 90 shares at 101 ticks fills 50 @100 and 40 @101; residual at 101 ask is 40. No incoming buy order remains.

**Acceptance evidence**
- Minimal reproducible fixture with explicit config/seed and expected outcome.
- Deterministic assertions for state, status, and output artifact integrity.
- Documentation of which phase introduces the scenario and how to rerun it.
### J.03 Noncrossing rest
A buy limit below best ask rests at its own price. Snapshot shows aggregate depth and order index contains its ID. Subsequent cancel removes both entry and, if empty, level.

**Acceptance evidence**
- Minimal reproducible fixture with explicit config/seed and expected outcome.
- Deterministic assertions for state, status, and output artifact integrity.
- Documentation of which phase introduces the scenario and how to rerun it.
### J.04 IOC residual
Aggressive IOC buy for 120 shares finds only 70 offered within limit. Execute 70 and cancel 50; never rest the 50.

**Acceptance evidence**
- Minimal reproducible fixture with explicit config/seed and expected outcome.
- Deterministic assertions for state, status, and output artifact integrity.
- Documentation of which phase introduces the scenario and how to rerun it.
### J.05 Market order empty
A market sell hits an empty bid book, executes zero shares, and returns terminal canceled/unfilled. Last trade and midpoint are unchanged/missing as appropriate.

**Acceptance evidence**
- Minimal reproducible fixture with explicit config/seed and expected outcome.
- Deterministic assertions for state, status, and output artifact integrity.
- Documentation of which phase introduces the scenario and how to rerun it.
### J.06 Cancel-fill tie
A cancel and crossing order arrive at identical timestamp. Lower global sequence executes first. Test both orderings to prove deterministic different outcomes.

**Acceptance evidence**
- Minimal reproducible fixture with explicit config/seed and expected outcome.
- Deterministic assertions for state, status, and output artifact integrity.
- Documentation of which phase introduces the scenario and how to rerun it.
### J.07 Ledger check
Trade 10 shares at $150.00 with one-cent ticks. Buyer inventory +10 and buyer cash -$1500 before fees; seller inventory -10 and seller cash +$1500. System-wide sum unchanged.

**Acceptance evidence**
- Minimal reproducible fixture with explicit config/seed and expected outcome.
- Deterministic assertions for state, status, and output artifact integrity.
- Documentation of which phase introduces the scenario and how to rerun it.
### J.08 Replay mutation
Replay canonical exchange inputs and verify fills/book checksum. Flip one order quantity in a copied trace; replay must fail with first affected sequence rather than passing solely because header digest was copied.

**Acceptance evidence**
- Minimal reproducible fixture with explicit config/seed and expected outcome.
- Deterministic assertions for state, status, and output artifact integrity.
- Documentation of which phase introduces the scenario and how to rerun it.
### J.09 Liquidity shock
Disable half maker population for a bounded interval and observe book thinning or increased spread if generated flow supports it. The simulator must not force price/spread outcomes.

**Acceptance evidence**
- Minimal reproducible fixture with explicit config/seed and expected outcome.
- Deterministic assertions for state, status, and output artifact integrity.
- Documentation of which phase introduces the scenario and how to rerun it.
### J.10 Paired comparison
Generate 200 shared scenario seeds; run baseline and inventory-aware maker once per seed. Compare per-seed P&L differences and tails, retaining failed seeds and outcome quality flags.

**Acceptance evidence**
- Minimal reproducible fixture with explicit config/seed and expected outcome.
- Deterministic assertions for state, status, and output artifact integrity.
- Documentation of which phase introduces the scenario and how to rerun it.
## Appendix K. Failure modes and recovery instructions
### K.01 Native assertion failure
Stop run with invariant violation code and causal event location. Preserve an incomplete manifest plus latest valid command log; do not mark run successful.

**Acceptance evidence**
- Minimal reproducible fixture with explicit config/seed and expected outcome.
- Deterministic assertions for state, status, and output artifact integrity.
- Documentation of which phase introduces the scenario and how to rerun it.
### K.02 Artifact write failure
Fail finalize phase, keep temp directory with diagnostic reason, and do not publish an apparently complete manifest.

**Acceptance evidence**
- Minimal reproducible fixture with explicit config/seed and expected outcome.
- Deterministic assertions for state, status, and output artifact integrity.
- Documentation of which phase introduces the scenario and how to rerun it.
### K.03 Worker terminated
On restart, reconcile pending/running job leases; mark interrupted runs retriable according to idempotence policy without overwriting previous finalized artifacts.

**Acceptance evidence**
- Minimal reproducible fixture with explicit config/seed and expected outcome.
- Deterministic assertions for state, status, and output artifact integrity.
- Documentation of which phase introduces the scenario and how to rerun it.
### K.04 Unsupported schema
Reject explicitly and include supported versions. Do not reinterpret unknown event types as generic events.

**Acceptance evidence**
- Minimal reproducible fixture with explicit config/seed and expected outcome.
- Deterministic assertions for state, status, and output artifact integrity.
- Documentation of which phase introduces the scenario and how to rerun it.
### K.05 Bad AI output
Pydantic validation rejects incompatible fields or out-of-bounds numbers; agent can revise specification within bounded retry budget.

**Acceptance evidence**
- Minimal reproducible fixture with explicit config/seed and expected outcome.
- Deterministic assertions for state, status, and output artifact integrity.
- Documentation of which phase introduces the scenario and how to rerun it.
### K.06 Empty liquidity
Metrics requiring midpoint/spread must be absent/null with flagged reason. Do not convert missing data to zero spread.

**Acceptance evidence**
- Minimal reproducible fixture with explicit config/seed and expected outcome.
- Deterministic assertions for state, status, and output artifact integrity.
- Documentation of which phase introduces the scenario and how to rerun it.
### K.07 Seed mismatch
If paired runs do not share the intended exogenous seed mapping, mark comparison as unpaired or invalid, not as paired.

**Acceptance evidence**
- Minimal reproducible fixture with explicit config/seed and expected outcome.
- Deterministic assertions for state, status, and output artifact integrity.
- Documentation of which phase introduces the scenario and how to rerun it.
### K.08 Conflicting version
If run manifests differ in metric definitions, refuse direct comparison until conversion rules are defined.

**Acceptance evidence**
- Minimal reproducible fixture with explicit config/seed and expected outcome.
- Deterministic assertions for state, status, and output artifact integrity.
- Documentation of which phase introduces the scenario and how to rerun it.
### K.09 Non-reproducible native run
Compare config digest, compiler flags, seed stream initialization, canonical events, and first divergence; never mask differences by increasing numeric tolerance of integer matching outputs.

**Acceptance evidence**
- Minimal reproducible fixture with explicit config/seed and expected outcome.
- Deterministic assertions for state, status, and output artifact integrity.
- Documentation of which phase introduces the scenario and how to rerun it.
### K.10 Over-budget scenario
Reject before queueing when estimated workload exceeds limits; if runtime still exceeds limits, stop safely and persist partial diagnostic evidence.

**Acceptance evidence**
- Minimal reproducible fixture with explicit config/seed and expected outcome.
- Deterministic assertions for state, status, and output artifact integrity.
- Documentation of which phase introduces the scenario and how to rerun it.
## Appendix L. Inter-module compatibility contracts
### L.01 Kernel to Exchange
- **Input semantics:** The Kernel -> Exchange integration MUST declare and test input semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying input semantics when Kernel produces valid inputs and Exchange consumes them.
  - Create one fixture verifying input semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a input semantics regression.
- **Output semantics:** The Kernel -> Exchange integration MUST declare and test output semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying output semantics when Kernel produces valid inputs and Exchange consumes them.
  - Create one fixture verifying output semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a output semantics regression.
- **Failure semantics:** The Kernel -> Exchange integration MUST declare and test failure semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying failure semantics when Kernel produces valid inputs and Exchange consumes them.
  - Create one fixture verifying failure semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a failure semantics regression.
- **Ordering guarantee:** The Kernel -> Exchange integration MUST declare and test ordering guarantee explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying ordering guarantee when Kernel produces valid inputs and Exchange consumes them.
  - Create one fixture verifying ordering guarantee when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a ordering guarantee regression.
- **Data ownership:** The Kernel -> Exchange integration MUST declare and test data ownership explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying data ownership when Kernel produces valid inputs and Exchange consumes them.
  - Create one fixture verifying data ownership when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a data ownership regression.
- **Version compatibility:** The Kernel -> Exchange integration MUST declare and test version compatibility explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying version compatibility when Kernel produces valid inputs and Exchange consumes them.
  - Create one fixture verifying version compatibility when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a version compatibility regression.
- **Performance boundary:** The Kernel -> Exchange integration MUST declare and test performance boundary explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying performance boundary when Kernel produces valid inputs and Exchange consumes them.
  - Create one fixture verifying performance boundary when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a performance boundary regression.
- **Test fixture:** The Kernel -> Exchange integration MUST declare and test test fixture explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying test fixture when Kernel produces valid inputs and Exchange consumes them.
  - Create one fixture verifying test fixture when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a test fixture regression.
- **Observability:** The Kernel -> Exchange integration MUST declare and test observability explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying observability when Kernel produces valid inputs and Exchange consumes them.
  - Create one fixture verifying observability when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a observability regression.
- **Forbidden coupling:** The Kernel -> Exchange integration MUST declare and test forbidden coupling explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying forbidden coupling when Kernel produces valid inputs and Exchange consumes them.
  - Create one fixture verifying forbidden coupling when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a forbidden coupling regression.
### L.02 Exchange to Accounting
- **Input semantics:** The Exchange -> Accounting integration MUST declare and test input semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying input semantics when Exchange produces valid inputs and Accounting consumes them.
  - Create one fixture verifying input semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a input semantics regression.
- **Output semantics:** The Exchange -> Accounting integration MUST declare and test output semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying output semantics when Exchange produces valid inputs and Accounting consumes them.
  - Create one fixture verifying output semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a output semantics regression.
- **Failure semantics:** The Exchange -> Accounting integration MUST declare and test failure semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying failure semantics when Exchange produces valid inputs and Accounting consumes them.
  - Create one fixture verifying failure semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a failure semantics regression.
- **Ordering guarantee:** The Exchange -> Accounting integration MUST declare and test ordering guarantee explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying ordering guarantee when Exchange produces valid inputs and Accounting consumes them.
  - Create one fixture verifying ordering guarantee when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a ordering guarantee regression.
- **Data ownership:** The Exchange -> Accounting integration MUST declare and test data ownership explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying data ownership when Exchange produces valid inputs and Accounting consumes them.
  - Create one fixture verifying data ownership when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a data ownership regression.
- **Version compatibility:** The Exchange -> Accounting integration MUST declare and test version compatibility explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying version compatibility when Exchange produces valid inputs and Accounting consumes them.
  - Create one fixture verifying version compatibility when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a version compatibility regression.
- **Performance boundary:** The Exchange -> Accounting integration MUST declare and test performance boundary explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying performance boundary when Exchange produces valid inputs and Accounting consumes them.
  - Create one fixture verifying performance boundary when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a performance boundary regression.
- **Test fixture:** The Exchange -> Accounting integration MUST declare and test test fixture explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying test fixture when Exchange produces valid inputs and Accounting consumes them.
  - Create one fixture verifying test fixture when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a test fixture regression.
- **Observability:** The Exchange -> Accounting integration MUST declare and test observability explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying observability when Exchange produces valid inputs and Accounting consumes them.
  - Create one fixture verifying observability when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a observability regression.
- **Forbidden coupling:** The Exchange -> Accounting integration MUST declare and test forbidden coupling explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying forbidden coupling when Exchange produces valid inputs and Accounting consumes them.
  - Create one fixture verifying forbidden coupling when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a forbidden coupling regression.
### L.03 Exchange to Telemetry
- **Input semantics:** The Exchange -> Telemetry integration MUST declare and test input semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying input semantics when Exchange produces valid inputs and Telemetry consumes them.
  - Create one fixture verifying input semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a input semantics regression.
- **Output semantics:** The Exchange -> Telemetry integration MUST declare and test output semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying output semantics when Exchange produces valid inputs and Telemetry consumes them.
  - Create one fixture verifying output semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a output semantics regression.
- **Failure semantics:** The Exchange -> Telemetry integration MUST declare and test failure semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying failure semantics when Exchange produces valid inputs and Telemetry consumes them.
  - Create one fixture verifying failure semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a failure semantics regression.
- **Ordering guarantee:** The Exchange -> Telemetry integration MUST declare and test ordering guarantee explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying ordering guarantee when Exchange produces valid inputs and Telemetry consumes them.
  - Create one fixture verifying ordering guarantee when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a ordering guarantee regression.
- **Data ownership:** The Exchange -> Telemetry integration MUST declare and test data ownership explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying data ownership when Exchange produces valid inputs and Telemetry consumes them.
  - Create one fixture verifying data ownership when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a data ownership regression.
- **Version compatibility:** The Exchange -> Telemetry integration MUST declare and test version compatibility explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying version compatibility when Exchange produces valid inputs and Telemetry consumes them.
  - Create one fixture verifying version compatibility when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a version compatibility regression.
- **Performance boundary:** The Exchange -> Telemetry integration MUST declare and test performance boundary explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying performance boundary when Exchange produces valid inputs and Telemetry consumes them.
  - Create one fixture verifying performance boundary when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a performance boundary regression.
- **Test fixture:** The Exchange -> Telemetry integration MUST declare and test test fixture explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying test fixture when Exchange produces valid inputs and Telemetry consumes them.
  - Create one fixture verifying test fixture when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a test fixture regression.
- **Observability:** The Exchange -> Telemetry integration MUST declare and test observability explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying observability when Exchange produces valid inputs and Telemetry consumes them.
  - Create one fixture verifying observability when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a observability regression.
- **Forbidden coupling:** The Exchange -> Telemetry integration MUST declare and test forbidden coupling explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying forbidden coupling when Exchange produces valid inputs and Telemetry consumes them.
  - Create one fixture verifying forbidden coupling when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a forbidden coupling regression.
### L.04 Kernel to Agents
- **Input semantics:** The Kernel -> Agents integration MUST declare and test input semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying input semantics when Kernel produces valid inputs and Agents consumes them.
  - Create one fixture verifying input semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a input semantics regression.
- **Output semantics:** The Kernel -> Agents integration MUST declare and test output semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying output semantics when Kernel produces valid inputs and Agents consumes them.
  - Create one fixture verifying output semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a output semantics regression.
- **Failure semantics:** The Kernel -> Agents integration MUST declare and test failure semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying failure semantics when Kernel produces valid inputs and Agents consumes them.
  - Create one fixture verifying failure semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a failure semantics regression.
- **Ordering guarantee:** The Kernel -> Agents integration MUST declare and test ordering guarantee explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying ordering guarantee when Kernel produces valid inputs and Agents consumes them.
  - Create one fixture verifying ordering guarantee when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a ordering guarantee regression.
- **Data ownership:** The Kernel -> Agents integration MUST declare and test data ownership explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying data ownership when Kernel produces valid inputs and Agents consumes them.
  - Create one fixture verifying data ownership when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a data ownership regression.
- **Version compatibility:** The Kernel -> Agents integration MUST declare and test version compatibility explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying version compatibility when Kernel produces valid inputs and Agents consumes them.
  - Create one fixture verifying version compatibility when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a version compatibility regression.
- **Performance boundary:** The Kernel -> Agents integration MUST declare and test performance boundary explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying performance boundary when Kernel produces valid inputs and Agents consumes them.
  - Create one fixture verifying performance boundary when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a performance boundary regression.
- **Test fixture:** The Kernel -> Agents integration MUST declare and test test fixture explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying test fixture when Kernel produces valid inputs and Agents consumes them.
  - Create one fixture verifying test fixture when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a test fixture regression.
- **Observability:** The Kernel -> Agents integration MUST declare and test observability explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying observability when Kernel produces valid inputs and Agents consumes them.
  - Create one fixture verifying observability when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a observability regression.
- **Forbidden coupling:** The Kernel -> Agents integration MUST declare and test forbidden coupling explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying forbidden coupling when Kernel produces valid inputs and Agents consumes them.
  - Create one fixture verifying forbidden coupling when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a forbidden coupling regression.
### L.05 Scenario to Agents
- **Input semantics:** The Scenario -> Agents integration MUST declare and test input semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying input semantics when Scenario produces valid inputs and Agents consumes them.
  - Create one fixture verifying input semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a input semantics regression.
- **Output semantics:** The Scenario -> Agents integration MUST declare and test output semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying output semantics when Scenario produces valid inputs and Agents consumes them.
  - Create one fixture verifying output semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a output semantics regression.
- **Failure semantics:** The Scenario -> Agents integration MUST declare and test failure semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying failure semantics when Scenario produces valid inputs and Agents consumes them.
  - Create one fixture verifying failure semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a failure semantics regression.
- **Ordering guarantee:** The Scenario -> Agents integration MUST declare and test ordering guarantee explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying ordering guarantee when Scenario produces valid inputs and Agents consumes them.
  - Create one fixture verifying ordering guarantee when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a ordering guarantee regression.
- **Data ownership:** The Scenario -> Agents integration MUST declare and test data ownership explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying data ownership when Scenario produces valid inputs and Agents consumes them.
  - Create one fixture verifying data ownership when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a data ownership regression.
- **Version compatibility:** The Scenario -> Agents integration MUST declare and test version compatibility explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying version compatibility when Scenario produces valid inputs and Agents consumes them.
  - Create one fixture verifying version compatibility when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a version compatibility regression.
- **Performance boundary:** The Scenario -> Agents integration MUST declare and test performance boundary explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying performance boundary when Scenario produces valid inputs and Agents consumes them.
  - Create one fixture verifying performance boundary when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a performance boundary regression.
- **Test fixture:** The Scenario -> Agents integration MUST declare and test test fixture explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying test fixture when Scenario produces valid inputs and Agents consumes them.
  - Create one fixture verifying test fixture when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a test fixture regression.
- **Observability:** The Scenario -> Agents integration MUST declare and test observability explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying observability when Scenario produces valid inputs and Agents consumes them.
  - Create one fixture verifying observability when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a observability regression.
- **Forbidden coupling:** The Scenario -> Agents integration MUST declare and test forbidden coupling explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying forbidden coupling when Scenario produces valid inputs and Agents consumes them.
  - Create one fixture verifying forbidden coupling when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a forbidden coupling regression.
### L.06 Agents to Accounting
- **Input semantics:** The Agents -> Accounting integration MUST declare and test input semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying input semantics when Agents produces valid inputs and Accounting consumes them.
  - Create one fixture verifying input semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a input semantics regression.
- **Output semantics:** The Agents -> Accounting integration MUST declare and test output semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying output semantics when Agents produces valid inputs and Accounting consumes them.
  - Create one fixture verifying output semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a output semantics regression.
- **Failure semantics:** The Agents -> Accounting integration MUST declare and test failure semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying failure semantics when Agents produces valid inputs and Accounting consumes them.
  - Create one fixture verifying failure semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a failure semantics regression.
- **Ordering guarantee:** The Agents -> Accounting integration MUST declare and test ordering guarantee explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying ordering guarantee when Agents produces valid inputs and Accounting consumes them.
  - Create one fixture verifying ordering guarantee when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a ordering guarantee regression.
- **Data ownership:** The Agents -> Accounting integration MUST declare and test data ownership explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying data ownership when Agents produces valid inputs and Accounting consumes them.
  - Create one fixture verifying data ownership when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a data ownership regression.
- **Version compatibility:** The Agents -> Accounting integration MUST declare and test version compatibility explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying version compatibility when Agents produces valid inputs and Accounting consumes them.
  - Create one fixture verifying version compatibility when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a version compatibility regression.
- **Performance boundary:** The Agents -> Accounting integration MUST declare and test performance boundary explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying performance boundary when Agents produces valid inputs and Accounting consumes them.
  - Create one fixture verifying performance boundary when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a performance boundary regression.
- **Test fixture:** The Agents -> Accounting integration MUST declare and test test fixture explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying test fixture when Agents produces valid inputs and Accounting consumes them.
  - Create one fixture verifying test fixture when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a test fixture regression.
- **Observability:** The Agents -> Accounting integration MUST declare and test observability explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying observability when Agents produces valid inputs and Accounting consumes them.
  - Create one fixture verifying observability when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a observability regression.
- **Forbidden coupling:** The Agents -> Accounting integration MUST declare and test forbidden coupling explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying forbidden coupling when Agents produces valid inputs and Accounting consumes them.
  - Create one fixture verifying forbidden coupling when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a forbidden coupling regression.
### L.07 Telemetry to Replay
- **Input semantics:** The Telemetry -> Replay integration MUST declare and test input semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying input semantics when Telemetry produces valid inputs and Replay consumes them.
  - Create one fixture verifying input semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a input semantics regression.
- **Output semantics:** The Telemetry -> Replay integration MUST declare and test output semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying output semantics when Telemetry produces valid inputs and Replay consumes them.
  - Create one fixture verifying output semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a output semantics regression.
- **Failure semantics:** The Telemetry -> Replay integration MUST declare and test failure semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying failure semantics when Telemetry produces valid inputs and Replay consumes them.
  - Create one fixture verifying failure semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a failure semantics regression.
- **Ordering guarantee:** The Telemetry -> Replay integration MUST declare and test ordering guarantee explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying ordering guarantee when Telemetry produces valid inputs and Replay consumes them.
  - Create one fixture verifying ordering guarantee when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a ordering guarantee regression.
- **Data ownership:** The Telemetry -> Replay integration MUST declare and test data ownership explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying data ownership when Telemetry produces valid inputs and Replay consumes them.
  - Create one fixture verifying data ownership when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a data ownership regression.
- **Version compatibility:** The Telemetry -> Replay integration MUST declare and test version compatibility explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying version compatibility when Telemetry produces valid inputs and Replay consumes them.
  - Create one fixture verifying version compatibility when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a version compatibility regression.
- **Performance boundary:** The Telemetry -> Replay integration MUST declare and test performance boundary explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying performance boundary when Telemetry produces valid inputs and Replay consumes them.
  - Create one fixture verifying performance boundary when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a performance boundary regression.
- **Test fixture:** The Telemetry -> Replay integration MUST declare and test test fixture explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying test fixture when Telemetry produces valid inputs and Replay consumes them.
  - Create one fixture verifying test fixture when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a test fixture regression.
- **Observability:** The Telemetry -> Replay integration MUST declare and test observability explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying observability when Telemetry produces valid inputs and Replay consumes them.
  - Create one fixture verifying observability when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a observability regression.
- **Forbidden coupling:** The Telemetry -> Replay integration MUST declare and test forbidden coupling explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying forbidden coupling when Telemetry produces valid inputs and Replay consumes them.
  - Create one fixture verifying forbidden coupling when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a forbidden coupling regression.
### L.08 Simulation to pybind11
- **Input semantics:** The Simulation -> pybind11 integration MUST declare and test input semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying input semantics when Simulation produces valid inputs and pybind11 consumes them.
  - Create one fixture verifying input semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a input semantics regression.
- **Output semantics:** The Simulation -> pybind11 integration MUST declare and test output semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying output semantics when Simulation produces valid inputs and pybind11 consumes them.
  - Create one fixture verifying output semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a output semantics regression.
- **Failure semantics:** The Simulation -> pybind11 integration MUST declare and test failure semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying failure semantics when Simulation produces valid inputs and pybind11 consumes them.
  - Create one fixture verifying failure semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a failure semantics regression.
- **Ordering guarantee:** The Simulation -> pybind11 integration MUST declare and test ordering guarantee explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying ordering guarantee when Simulation produces valid inputs and pybind11 consumes them.
  - Create one fixture verifying ordering guarantee when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a ordering guarantee regression.
- **Data ownership:** The Simulation -> pybind11 integration MUST declare and test data ownership explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying data ownership when Simulation produces valid inputs and pybind11 consumes them.
  - Create one fixture verifying data ownership when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a data ownership regression.
- **Version compatibility:** The Simulation -> pybind11 integration MUST declare and test version compatibility explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying version compatibility when Simulation produces valid inputs and pybind11 consumes them.
  - Create one fixture verifying version compatibility when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a version compatibility regression.
- **Performance boundary:** The Simulation -> pybind11 integration MUST declare and test performance boundary explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying performance boundary when Simulation produces valid inputs and pybind11 consumes them.
  - Create one fixture verifying performance boundary when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a performance boundary regression.
- **Test fixture:** The Simulation -> pybind11 integration MUST declare and test test fixture explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying test fixture when Simulation produces valid inputs and pybind11 consumes them.
  - Create one fixture verifying test fixture when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a test fixture regression.
- **Observability:** The Simulation -> pybind11 integration MUST declare and test observability explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying observability when Simulation produces valid inputs and pybind11 consumes them.
  - Create one fixture verifying observability when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a observability regression.
- **Forbidden coupling:** The Simulation -> pybind11 integration MUST declare and test forbidden coupling explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying forbidden coupling when Simulation produces valid inputs and pybind11 consumes them.
  - Create one fixture verifying forbidden coupling when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a forbidden coupling regression.
### L.09 pybind11 to Research
- **Input semantics:** The pybind11 -> Research integration MUST declare and test input semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying input semantics when pybind11 produces valid inputs and Research consumes them.
  - Create one fixture verifying input semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a input semantics regression.
- **Output semantics:** The pybind11 -> Research integration MUST declare and test output semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying output semantics when pybind11 produces valid inputs and Research consumes them.
  - Create one fixture verifying output semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a output semantics regression.
- **Failure semantics:** The pybind11 -> Research integration MUST declare and test failure semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying failure semantics when pybind11 produces valid inputs and Research consumes them.
  - Create one fixture verifying failure semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a failure semantics regression.
- **Ordering guarantee:** The pybind11 -> Research integration MUST declare and test ordering guarantee explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying ordering guarantee when pybind11 produces valid inputs and Research consumes them.
  - Create one fixture verifying ordering guarantee when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a ordering guarantee regression.
- **Data ownership:** The pybind11 -> Research integration MUST declare and test data ownership explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying data ownership when pybind11 produces valid inputs and Research consumes them.
  - Create one fixture verifying data ownership when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a data ownership regression.
- **Version compatibility:** The pybind11 -> Research integration MUST declare and test version compatibility explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying version compatibility when pybind11 produces valid inputs and Research consumes them.
  - Create one fixture verifying version compatibility when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a version compatibility regression.
- **Performance boundary:** The pybind11 -> Research integration MUST declare and test performance boundary explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying performance boundary when pybind11 produces valid inputs and Research consumes them.
  - Create one fixture verifying performance boundary when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a performance boundary regression.
- **Test fixture:** The pybind11 -> Research integration MUST declare and test test fixture explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying test fixture when pybind11 produces valid inputs and Research consumes them.
  - Create one fixture verifying test fixture when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a test fixture regression.
- **Observability:** The pybind11 -> Research integration MUST declare and test observability explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying observability when pybind11 produces valid inputs and Research consumes them.
  - Create one fixture verifying observability when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a observability regression.
- **Forbidden coupling:** The pybind11 -> Research integration MUST declare and test forbidden coupling explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying forbidden coupling when pybind11 produces valid inputs and Research consumes them.
  - Create one fixture verifying forbidden coupling when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a forbidden coupling regression.
### L.10 Research to API
- **Input semantics:** The Research -> API integration MUST declare and test input semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying input semantics when Research produces valid inputs and API consumes them.
  - Create one fixture verifying input semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a input semantics regression.
- **Output semantics:** The Research -> API integration MUST declare and test output semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying output semantics when Research produces valid inputs and API consumes them.
  - Create one fixture verifying output semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a output semantics regression.
- **Failure semantics:** The Research -> API integration MUST declare and test failure semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying failure semantics when Research produces valid inputs and API consumes them.
  - Create one fixture verifying failure semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a failure semantics regression.
- **Ordering guarantee:** The Research -> API integration MUST declare and test ordering guarantee explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying ordering guarantee when Research produces valid inputs and API consumes them.
  - Create one fixture verifying ordering guarantee when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a ordering guarantee regression.
- **Data ownership:** The Research -> API integration MUST declare and test data ownership explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying data ownership when Research produces valid inputs and API consumes them.
  - Create one fixture verifying data ownership when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a data ownership regression.
- **Version compatibility:** The Research -> API integration MUST declare and test version compatibility explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying version compatibility when Research produces valid inputs and API consumes them.
  - Create one fixture verifying version compatibility when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a version compatibility regression.
- **Performance boundary:** The Research -> API integration MUST declare and test performance boundary explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying performance boundary when Research produces valid inputs and API consumes them.
  - Create one fixture verifying performance boundary when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a performance boundary regression.
- **Test fixture:** The Research -> API integration MUST declare and test test fixture explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying test fixture when Research produces valid inputs and API consumes them.
  - Create one fixture verifying test fixture when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a test fixture regression.
- **Observability:** The Research -> API integration MUST declare and test observability explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying observability when Research produces valid inputs and API consumes them.
  - Create one fixture verifying observability when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a observability regression.
- **Forbidden coupling:** The Research -> API integration MUST declare and test forbidden coupling explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying forbidden coupling when Research produces valid inputs and API consumes them.
  - Create one fixture verifying forbidden coupling when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a forbidden coupling regression.
### L.11 API to Worker
- **Input semantics:** The API -> Worker integration MUST declare and test input semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying input semantics when API produces valid inputs and Worker consumes them.
  - Create one fixture verifying input semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a input semantics regression.
- **Output semantics:** The API -> Worker integration MUST declare and test output semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying output semantics when API produces valid inputs and Worker consumes them.
  - Create one fixture verifying output semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a output semantics regression.
- **Failure semantics:** The API -> Worker integration MUST declare and test failure semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying failure semantics when API produces valid inputs and Worker consumes them.
  - Create one fixture verifying failure semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a failure semantics regression.
- **Ordering guarantee:** The API -> Worker integration MUST declare and test ordering guarantee explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying ordering guarantee when API produces valid inputs and Worker consumes them.
  - Create one fixture verifying ordering guarantee when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a ordering guarantee regression.
- **Data ownership:** The API -> Worker integration MUST declare and test data ownership explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying data ownership when API produces valid inputs and Worker consumes them.
  - Create one fixture verifying data ownership when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a data ownership regression.
- **Version compatibility:** The API -> Worker integration MUST declare and test version compatibility explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying version compatibility when API produces valid inputs and Worker consumes them.
  - Create one fixture verifying version compatibility when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a version compatibility regression.
- **Performance boundary:** The API -> Worker integration MUST declare and test performance boundary explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying performance boundary when API produces valid inputs and Worker consumes them.
  - Create one fixture verifying performance boundary when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a performance boundary regression.
- **Test fixture:** The API -> Worker integration MUST declare and test test fixture explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying test fixture when API produces valid inputs and Worker consumes them.
  - Create one fixture verifying test fixture when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a test fixture regression.
- **Observability:** The API -> Worker integration MUST declare and test observability explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying observability when API produces valid inputs and Worker consumes them.
  - Create one fixture verifying observability when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a observability regression.
- **Forbidden coupling:** The API -> Worker integration MUST declare and test forbidden coupling explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying forbidden coupling when API produces valid inputs and Worker consumes them.
  - Create one fixture verifying forbidden coupling when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a forbidden coupling regression.
### L.12 Worker to Artifacts
- **Input semantics:** The Worker -> Artifacts integration MUST declare and test input semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying input semantics when Worker produces valid inputs and Artifacts consumes them.
  - Create one fixture verifying input semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a input semantics regression.
- **Output semantics:** The Worker -> Artifacts integration MUST declare and test output semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying output semantics when Worker produces valid inputs and Artifacts consumes them.
  - Create one fixture verifying output semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a output semantics regression.
- **Failure semantics:** The Worker -> Artifacts integration MUST declare and test failure semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying failure semantics when Worker produces valid inputs and Artifacts consumes them.
  - Create one fixture verifying failure semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a failure semantics regression.
- **Ordering guarantee:** The Worker -> Artifacts integration MUST declare and test ordering guarantee explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying ordering guarantee when Worker produces valid inputs and Artifacts consumes them.
  - Create one fixture verifying ordering guarantee when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a ordering guarantee regression.
- **Data ownership:** The Worker -> Artifacts integration MUST declare and test data ownership explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying data ownership when Worker produces valid inputs and Artifacts consumes them.
  - Create one fixture verifying data ownership when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a data ownership regression.
- **Version compatibility:** The Worker -> Artifacts integration MUST declare and test version compatibility explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying version compatibility when Worker produces valid inputs and Artifacts consumes them.
  - Create one fixture verifying version compatibility when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a version compatibility regression.
- **Performance boundary:** The Worker -> Artifacts integration MUST declare and test performance boundary explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying performance boundary when Worker produces valid inputs and Artifacts consumes them.
  - Create one fixture verifying performance boundary when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a performance boundary regression.
- **Test fixture:** The Worker -> Artifacts integration MUST declare and test test fixture explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying test fixture when Worker produces valid inputs and Artifacts consumes them.
  - Create one fixture verifying test fixture when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a test fixture regression.
- **Observability:** The Worker -> Artifacts integration MUST declare and test observability explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying observability when Worker produces valid inputs and Artifacts consumes them.
  - Create one fixture verifying observability when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a observability regression.
- **Forbidden coupling:** The Worker -> Artifacts integration MUST declare and test forbidden coupling explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying forbidden coupling when Worker produces valid inputs and Artifacts consumes them.
  - Create one fixture verifying forbidden coupling when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a forbidden coupling regression.
### L.13 API to UI
- **Input semantics:** The API -> UI integration MUST declare and test input semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying input semantics when API produces valid inputs and UI consumes them.
  - Create one fixture verifying input semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a input semantics regression.
- **Output semantics:** The API -> UI integration MUST declare and test output semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying output semantics when API produces valid inputs and UI consumes them.
  - Create one fixture verifying output semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a output semantics regression.
- **Failure semantics:** The API -> UI integration MUST declare and test failure semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying failure semantics when API produces valid inputs and UI consumes them.
  - Create one fixture verifying failure semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a failure semantics regression.
- **Ordering guarantee:** The API -> UI integration MUST declare and test ordering guarantee explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying ordering guarantee when API produces valid inputs and UI consumes them.
  - Create one fixture verifying ordering guarantee when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a ordering guarantee regression.
- **Data ownership:** The API -> UI integration MUST declare and test data ownership explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying data ownership when API produces valid inputs and UI consumes them.
  - Create one fixture verifying data ownership when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a data ownership regression.
- **Version compatibility:** The API -> UI integration MUST declare and test version compatibility explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying version compatibility when API produces valid inputs and UI consumes them.
  - Create one fixture verifying version compatibility when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a version compatibility regression.
- **Performance boundary:** The API -> UI integration MUST declare and test performance boundary explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying performance boundary when API produces valid inputs and UI consumes them.
  - Create one fixture verifying performance boundary when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a performance boundary regression.
- **Test fixture:** The API -> UI integration MUST declare and test test fixture explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying test fixture when API produces valid inputs and UI consumes them.
  - Create one fixture verifying test fixture when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a test fixture regression.
- **Observability:** The API -> UI integration MUST declare and test observability explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying observability when API produces valid inputs and UI consumes them.
  - Create one fixture verifying observability when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a observability regression.
- **Forbidden coupling:** The API -> UI integration MUST declare and test forbidden coupling explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying forbidden coupling when API produces valid inputs and UI consumes them.
  - Create one fixture verifying forbidden coupling when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a forbidden coupling regression.
### L.14 Research to AI
- **Input semantics:** The Research -> AI integration MUST declare and test input semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying input semantics when Research produces valid inputs and AI consumes them.
  - Create one fixture verifying input semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a input semantics regression.
- **Output semantics:** The Research -> AI integration MUST declare and test output semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying output semantics when Research produces valid inputs and AI consumes them.
  - Create one fixture verifying output semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a output semantics regression.
- **Failure semantics:** The Research -> AI integration MUST declare and test failure semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying failure semantics when Research produces valid inputs and AI consumes them.
  - Create one fixture verifying failure semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a failure semantics regression.
- **Ordering guarantee:** The Research -> AI integration MUST declare and test ordering guarantee explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying ordering guarantee when Research produces valid inputs and AI consumes them.
  - Create one fixture verifying ordering guarantee when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a ordering guarantee regression.
- **Data ownership:** The Research -> AI integration MUST declare and test data ownership explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying data ownership when Research produces valid inputs and AI consumes them.
  - Create one fixture verifying data ownership when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a data ownership regression.
- **Version compatibility:** The Research -> AI integration MUST declare and test version compatibility explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying version compatibility when Research produces valid inputs and AI consumes them.
  - Create one fixture verifying version compatibility when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a version compatibility regression.
- **Performance boundary:** The Research -> AI integration MUST declare and test performance boundary explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying performance boundary when Research produces valid inputs and AI consumes them.
  - Create one fixture verifying performance boundary when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a performance boundary regression.
- **Test fixture:** The Research -> AI integration MUST declare and test test fixture explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying test fixture when Research produces valid inputs and AI consumes them.
  - Create one fixture verifying test fixture when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a test fixture regression.
- **Observability:** The Research -> AI integration MUST declare and test observability explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying observability when Research produces valid inputs and AI consumes them.
  - Create one fixture verifying observability when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a observability regression.
- **Forbidden coupling:** The Research -> AI integration MUST declare and test forbidden coupling explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying forbidden coupling when Research produces valid inputs and AI consumes them.
  - Create one fixture verifying forbidden coupling when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a forbidden coupling regression.
### L.15 AI to API
- **Input semantics:** The AI -> API integration MUST declare and test input semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying input semantics when AI produces valid inputs and API consumes them.
  - Create one fixture verifying input semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a input semantics regression.
- **Output semantics:** The AI -> API integration MUST declare and test output semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying output semantics when AI produces valid inputs and API consumes them.
  - Create one fixture verifying output semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a output semantics regression.
- **Failure semantics:** The AI -> API integration MUST declare and test failure semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying failure semantics when AI produces valid inputs and API consumes them.
  - Create one fixture verifying failure semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a failure semantics regression.
- **Ordering guarantee:** The AI -> API integration MUST declare and test ordering guarantee explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying ordering guarantee when AI produces valid inputs and API consumes them.
  - Create one fixture verifying ordering guarantee when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a ordering guarantee regression.
- **Data ownership:** The AI -> API integration MUST declare and test data ownership explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying data ownership when AI produces valid inputs and API consumes them.
  - Create one fixture verifying data ownership when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a data ownership regression.
- **Version compatibility:** The AI -> API integration MUST declare and test version compatibility explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying version compatibility when AI produces valid inputs and API consumes them.
  - Create one fixture verifying version compatibility when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a version compatibility regression.
- **Performance boundary:** The AI -> API integration MUST declare and test performance boundary explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying performance boundary when AI produces valid inputs and API consumes them.
  - Create one fixture verifying performance boundary when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a performance boundary regression.
- **Test fixture:** The AI -> API integration MUST declare and test test fixture explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying test fixture when AI produces valid inputs and API consumes them.
  - Create one fixture verifying test fixture when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a test fixture regression.
- **Observability:** The AI -> API integration MUST declare and test observability explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying observability when AI produces valid inputs and API consumes them.
  - Create one fixture verifying observability when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a observability regression.
- **Forbidden coupling:** The AI -> API integration MUST declare and test forbidden coupling explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying forbidden coupling when AI produces valid inputs and API consumes them.
  - Create one fixture verifying forbidden coupling when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a forbidden coupling regression.
### L.16 Replay to UI
- **Input semantics:** The Replay -> UI integration MUST declare and test input semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying input semantics when Replay produces valid inputs and UI consumes them.
  - Create one fixture verifying input semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a input semantics regression.
- **Output semantics:** The Replay -> UI integration MUST declare and test output semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying output semantics when Replay produces valid inputs and UI consumes them.
  - Create one fixture verifying output semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a output semantics regression.
- **Failure semantics:** The Replay -> UI integration MUST declare and test failure semantics explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying failure semantics when Replay produces valid inputs and UI consumes them.
  - Create one fixture verifying failure semantics when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a failure semantics regression.
- **Ordering guarantee:** The Replay -> UI integration MUST declare and test ordering guarantee explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying ordering guarantee when Replay produces valid inputs and UI consumes them.
  - Create one fixture verifying ordering guarantee when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a ordering guarantee regression.
- **Data ownership:** The Replay -> UI integration MUST declare and test data ownership explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying data ownership when Replay produces valid inputs and UI consumes them.
  - Create one fixture verifying data ownership when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a data ownership regression.
- **Version compatibility:** The Replay -> UI integration MUST declare and test version compatibility explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying version compatibility when Replay produces valid inputs and UI consumes them.
  - Create one fixture verifying version compatibility when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a version compatibility regression.
- **Performance boundary:** The Replay -> UI integration MUST declare and test performance boundary explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying performance boundary when Replay produces valid inputs and UI consumes them.
  - Create one fixture verifying performance boundary when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a performance boundary regression.
- **Test fixture:** The Replay -> UI integration MUST declare and test test fixture explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying test fixture when Replay produces valid inputs and UI consumes them.
  - Create one fixture verifying test fixture when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a test fixture regression.
- **Observability:** The Replay -> UI integration MUST declare and test observability explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying observability when Replay produces valid inputs and UI consumes them.
  - Create one fixture verifying observability when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a observability regression.
- **Forbidden coupling:** The Replay -> UI integration MUST declare and test forbidden coupling explicitly, using typed data, stable error codes, and the documented event sequence.
  - Create one fixture verifying forbidden coupling when Replay produces valid inputs and UI consumes them.
  - Create one fixture verifying forbidden coupling when the source data is invalid, missing, stale, or exceeds configured bounds.
  - Record the first event/run ID or schema revision needed to reproduce a forbidden coupling regression.
## Appendix M. Benchmark and golden-run workload manifest
### M.01 Empty Market
- **Configuration:** Define a versioned fixture for empty market; include symbol, tick size, seed, agent mix, order budget, and log granularity.
- **Functional correctness:** Require passing invariants after each relevant exchange transition in debug runs.
- **Determinism:** Save canonical input trace and expected summary/book/ledger digests for the supported platform.
- **Performance:** Capture event count, match rate, p50/p95/p99 processing time when appropriate, memory peak, and output size.
- **Comparability:** Record source revision, compiler, flags, machine CPU, units, and workload input distribution.
- **Interpretation:** Identify what the workload does and does not tell us about market realism or actual exchange throughput.
### M.02 One Quote On Each Side
- **Configuration:** Define a versioned fixture for one quote on each side; include symbol, tick size, seed, agent mix, order budget, and log granularity.
- **Functional correctness:** Require passing invariants after each relevant exchange transition in debug runs.
- **Determinism:** Save canonical input trace and expected summary/book/ledger digests for the supported platform.
- **Performance:** Capture event count, match rate, p50/p95/p99 processing time when appropriate, memory peak, and output size.
- **Comparability:** Record source revision, compiler, flags, machine CPU, units, and workload input distribution.
- **Interpretation:** Identify what the workload does and does not tell us about market realism or actual exchange throughput.
### M.03 Dense 10-Level Book
- **Configuration:** Define a versioned fixture for dense 10-level book; include symbol, tick size, seed, agent mix, order budget, and log granularity.
- **Functional correctness:** Require passing invariants after each relevant exchange transition in debug runs.
- **Determinism:** Save canonical input trace and expected summary/book/ledger digests for the supported platform.
- **Performance:** Capture event count, match rate, p50/p95/p99 processing time when appropriate, memory peak, and output size.
- **Comparability:** Record source revision, compiler, flags, machine CPU, units, and workload input distribution.
- **Interpretation:** Identify what the workload does and does not tell us about market realism or actual exchange throughput.
### M.04 All Market Orders
- **Configuration:** Define a versioned fixture for all market orders; include symbol, tick size, seed, agent mix, order budget, and log granularity.
- **Functional correctness:** Require passing invariants after each relevant exchange transition in debug runs.
- **Determinism:** Save canonical input trace and expected summary/book/ledger digests for the supported platform.
- **Performance:** Capture event count, match rate, p50/p95/p99 processing time when appropriate, memory peak, and output size.
- **Comparability:** Record source revision, compiler, flags, machine CPU, units, and workload input distribution.
- **Interpretation:** Identify what the workload does and does not tell us about market realism or actual exchange throughput.
### M.05 All Noncrossing Limits
- **Configuration:** Define a versioned fixture for all noncrossing limits; include symbol, tick size, seed, agent mix, order budget, and log granularity.
- **Functional correctness:** Require passing invariants after each relevant exchange transition in debug runs.
- **Determinism:** Save canonical input trace and expected summary/book/ledger digests for the supported platform.
- **Performance:** Capture event count, match rate, p50/p95/p99 processing time when appropriate, memory peak, and output size.
- **Comparability:** Record source revision, compiler, flags, machine CPU, units, and workload input distribution.
- **Interpretation:** Identify what the workload does and does not tell us about market realism or actual exchange throughput.
### M.06 Cancel-Heavy Burst
- **Configuration:** Define a versioned fixture for cancel-heavy burst; include symbol, tick size, seed, agent mix, order budget, and log granularity.
- **Functional correctness:** Require passing invariants after each relevant exchange transition in debug runs.
- **Determinism:** Save canonical input trace and expected summary/book/ledger digests for the supported platform.
- **Performance:** Capture event count, match rate, p50/p95/p99 processing time when appropriate, memory peak, and output size.
- **Comparability:** Record source revision, compiler, flags, machine CPU, units, and workload input distribution.
- **Interpretation:** Identify what the workload does and does not tell us about market realism or actual exchange throughput.
### M.07 100% Aggressive Flow
- **Configuration:** Define a versioned fixture for 100% aggressive flow; include symbol, tick size, seed, agent mix, order budget, and log granularity.
- **Functional correctness:** Require passing invariants after each relevant exchange transition in debug runs.
- **Determinism:** Save canonical input trace and expected summary/book/ledger digests for the supported platform.
- **Performance:** Capture event count, match rate, p50/p95/p99 processing time when appropriate, memory peak, and output size.
- **Comparability:** Record source revision, compiler, flags, machine CPU, units, and workload input distribution.
- **Interpretation:** Identify what the workload does and does not tell us about market realism or actual exchange throughput.
### M.08 Mixed Fifo Same-Price Queue
- **Configuration:** Define a versioned fixture for mixed FIFO same-price queue; include symbol, tick size, seed, agent mix, order budget, and log granularity.
- **Functional correctness:** Require passing invariants after each relevant exchange transition in debug runs.
- **Determinism:** Save canonical input trace and expected summary/book/ledger digests for the supported platform.
- **Performance:** Capture event count, match rate, p50/p95/p99 processing time when appropriate, memory peak, and output size.
- **Comparability:** Record source revision, compiler, flags, machine CPU, units, and workload input distribution.
- **Interpretation:** Identify what the workload does and does not tell us about market realism or actual exchange throughput.
### M.09 High-Volatility Shock
- **Configuration:** Define a versioned fixture for high-volatility shock; include symbol, tick size, seed, agent mix, order budget, and log granularity.
- **Functional correctness:** Require passing invariants after each relevant exchange transition in debug runs.
- **Determinism:** Save canonical input trace and expected summary/book/ledger digests for the supported platform.
- **Performance:** Capture event count, match rate, p50/p95/p99 processing time when appropriate, memory peak, and output size.
- **Comparability:** Record source revision, compiler, flags, machine CPU, units, and workload input distribution.
- **Interpretation:** Identify what the workload does and does not tell us about market realism or actual exchange throughput.
### M.10 Liquidity Withdrawal And Recovery
- **Configuration:** Define a versioned fixture for liquidity withdrawal and recovery; include symbol, tick size, seed, agent mix, order budget, and log granularity.
- **Functional correctness:** Require passing invariants after each relevant exchange transition in debug runs.
- **Determinism:** Save canonical input trace and expected summary/book/ledger digests for the supported platform.
- **Performance:** Capture event count, match rate, p50/p95/p99 processing time when appropriate, memory peak, and output size.
- **Comparability:** Record source revision, compiler, flags, machine CPU, units, and workload input distribution.
- **Interpretation:** Identify what the workload does and does not tell us about market realism or actual exchange throughput.
### M.11 Stale Market-Data Delivery
- **Configuration:** Define a versioned fixture for stale market-data delivery; include symbol, tick size, seed, agent mix, order budget, and log granularity.
- **Functional correctness:** Require passing invariants after each relevant exchange transition in debug runs.
- **Determinism:** Save canonical input trace and expected summary/book/ledger digests for the supported platform.
- **Performance:** Capture event count, match rate, p50/p95/p99 processing time when appropriate, memory peak, and output size.
- **Comparability:** Record source revision, compiler, flags, machine CPU, units, and workload input distribution.
- **Interpretation:** Identify what the workload does and does not tell us about market realism or actual exchange throughput.
### M.12 High Agent Count
- **Configuration:** Define a versioned fixture for high agent count; include symbol, tick size, seed, agent mix, order budget, and log granularity.
- **Functional correctness:** Require passing invariants after each relevant exchange transition in debug runs.
- **Determinism:** Save canonical input trace and expected summary/book/ledger digests for the supported platform.
- **Performance:** Capture event count, match rate, p50/p95/p99 processing time when appropriate, memory peak, and output size.
- **Comparability:** Record source revision, compiler, flags, machine CPU, units, and workload input distribution.
- **Interpretation:** Identify what the workload does and does not tell us about market realism or actual exchange throughput.
### M.13 Long Logical-Time Horizon
- **Configuration:** Define a versioned fixture for long logical-time horizon; include symbol, tick size, seed, agent mix, order budget, and log granularity.
- **Functional correctness:** Require passing invariants after each relevant exchange transition in debug runs.
- **Determinism:** Save canonical input trace and expected summary/book/ledger digests for the supported platform.
- **Performance:** Capture event count, match rate, p50/p95/p99 processing time when appropriate, memory peak, and output size.
- **Comparability:** Record source revision, compiler, flags, machine CPU, units, and workload input distribution.
- **Interpretation:** Identify what the workload does and does not tell us about market realism or actual exchange throughput.
### M.14 Zero Trades
- **Configuration:** Define a versioned fixture for zero trades; include symbol, tick size, seed, agent mix, order budget, and log granularity.
- **Functional correctness:** Require passing invariants after each relevant exchange transition in debug runs.
- **Determinism:** Save canonical input trace and expected summary/book/ledger digests for the supported platform.
- **Performance:** Capture event count, match rate, p50/p95/p99 processing time when appropriate, memory peak, and output size.
- **Comparability:** Record source revision, compiler, flags, machine CPU, units, and workload input distribution.
- **Interpretation:** Identify what the workload does and does not tell us about market realism or actual exchange throughput.
### M.15 Maximum Allowed Events
- **Configuration:** Define a versioned fixture for maximum allowed events; include symbol, tick size, seed, agent mix, order budget, and log granularity.
- **Functional correctness:** Require passing invariants after each relevant exchange transition in debug runs.
- **Determinism:** Save canonical input trace and expected summary/book/ledger digests for the supported platform.
- **Performance:** Capture event count, match rate, p50/p95/p99 processing time when appropriate, memory peak, and output size.
- **Comparability:** Record source revision, compiler, flags, machine CPU, units, and workload input distribution.
- **Interpretation:** Identify what the workload does and does not tell us about market realism or actual exchange throughput.
### M.16 Many Small Partial Fills
- **Configuration:** Define a versioned fixture for many small partial fills; include symbol, tick size, seed, agent mix, order budget, and log granularity.
- **Functional correctness:** Require passing invariants after each relevant exchange transition in debug runs.
- **Determinism:** Save canonical input trace and expected summary/book/ledger digests for the supported platform.
- **Performance:** Capture event count, match rate, p50/p95/p99 processing time when appropriate, memory peak, and output size.
- **Comparability:** Record source revision, compiler, flags, machine CPU, units, and workload input distribution.
- **Interpretation:** Identify what the workload does and does not tell us about market realism or actual exchange throughput.
### M.17 Single Large Sweeping Order
- **Configuration:** Define a versioned fixture for single large sweeping order; include symbol, tick size, seed, agent mix, order budget, and log granularity.
- **Functional correctness:** Require passing invariants after each relevant exchange transition in debug runs.
- **Determinism:** Save canonical input trace and expected summary/book/ledger digests for the supported platform.
- **Performance:** Capture event count, match rate, p50/p95/p99 processing time when appropriate, memory peak, and output size.
- **Comparability:** Record source revision, compiler, flags, machine CPU, units, and workload input distribution.
- **Interpretation:** Identify what the workload does and does not tell us about market realism or actual exchange throughput.
### M.18 Two Simultaneous Cancels
- **Configuration:** Define a versioned fixture for two simultaneous cancels; include symbol, tick size, seed, agent mix, order budget, and log granularity.
- **Functional correctness:** Require passing invariants after each relevant exchange transition in debug runs.
- **Determinism:** Save canonical input trace and expected summary/book/ledger digests for the supported platform.
- **Performance:** Capture event count, match rate, p50/p95/p99 processing time when appropriate, memory peak, and output size.
- **Comparability:** Record source revision, compiler, flags, machine CPU, units, and workload input distribution.
- **Interpretation:** Identify what the workload does and does not tell us about market realism or actual exchange throughput.
### M.19 Repeated Run Restart
- **Configuration:** Define a versioned fixture for repeated run restart; include symbol, tick size, seed, agent mix, order budget, and log granularity.
- **Functional correctness:** Require passing invariants after each relevant exchange transition in debug runs.
- **Determinism:** Save canonical input trace and expected summary/book/ledger digests for the supported platform.
- **Performance:** Capture event count, match rate, p50/p95/p99 processing time when appropriate, memory peak, and output size.
- **Comparability:** Record source revision, compiler, flags, machine CPU, units, and workload input distribution.
- **Interpretation:** Identify what the workload does and does not tell us about market realism or actual exchange throughput.
### M.20 Replay Artifact Corruption
- **Configuration:** Define a versioned fixture for replay artifact corruption; include symbol, tick size, seed, agent mix, order budget, and log granularity.
- **Functional correctness:** Require passing invariants after each relevant exchange transition in debug runs.
- **Determinism:** Save canonical input trace and expected summary/book/ledger digests for the supported platform.
- **Performance:** Capture event count, match rate, p50/p95/p99 processing time when appropriate, memory peak, and output size.
- **Comparability:** Record source revision, compiler, flags, machine CPU, units, and workload input distribution.
- **Interpretation:** Identify what the workload does and does not tell us about market realism or actual exchange throughput.
## Appendix N. Field-level schema implementation and validation checklist
### Schema Order
#### `Order.id`
- **Purpose:** `id` participates in the declared `Order` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Order.owner`
- **Purpose:** `owner` participates in the declared `Order` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Order.symbol`
- **Purpose:** `symbol` participates in the declared `Order` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Order.side`
- **Purpose:** `side` participates in the declared `Order` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Order.type`
- **Purpose:** `type` participates in the declared `Order` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Order.limit_price_ticks`
- **Purpose:** `limit_price_ticks` participates in the declared `Order` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Order.original_qty`
- **Purpose:** `original_qty` participates in the declared `Order` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Order.remaining_qty`
- **Purpose:** `remaining_qty` participates in the declared `Order` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Order.tif`
- **Purpose:** `tif` participates in the declared `Order` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Order.arrival_time_ns`
- **Purpose:** `arrival_time_ns` participates in the declared `Order` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Order.arrival_sequence`
- **Purpose:** `arrival_sequence` participates in the declared `Order` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
### Schema Trade
#### `Trade.id`
- **Purpose:** `id` participates in the declared `Trade` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Trade.buy_order_id`
- **Purpose:** `buy_order_id` participates in the declared `Trade` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Trade.sell_order_id`
- **Purpose:** `sell_order_id` participates in the declared `Trade` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Trade.aggressor_order_id`
- **Purpose:** `aggressor_order_id` participates in the declared `Trade` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Trade.resting_order_id`
- **Purpose:** `resting_order_id` participates in the declared `Trade` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Trade.price_ticks`
- **Purpose:** `price_ticks` participates in the declared `Trade` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Trade.quantity`
- **Purpose:** `quantity` participates in the declared `Trade` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Trade.timestamp_ns`
- **Purpose:** `timestamp_ns` participates in the declared `Trade` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Trade.sequence`
- **Purpose:** `sequence` participates in the declared `Trade` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Trade.maker_agent_id`
- **Purpose:** `maker_agent_id` participates in the declared `Trade` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Trade.taker_agent_id`
- **Purpose:** `taker_agent_id` participates in the declared `Trade` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
### Schema Event
#### `Event.schema_version`
- **Purpose:** `schema_version` participates in the declared `Event` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Event.run_id`
- **Purpose:** `run_id` participates in the declared `Event` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Event.sequence`
- **Purpose:** `sequence` participates in the declared `Event` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Event.timestamp_ns`
- **Purpose:** `timestamp_ns` participates in the declared `Event` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Event.kind`
- **Purpose:** `kind` participates in the declared `Event` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Event.actor`
- **Purpose:** `actor` participates in the declared `Event` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Event.causation_id`
- **Purpose:** `causation_id` participates in the declared `Event` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Event.payload`
- **Purpose:** `payload` participates in the declared `Event` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Event.payload_digest`
- **Purpose:** `payload_digest` participates in the declared `Event` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
### Schema Manifest
#### `Manifest.schema_version`
- **Purpose:** `schema_version` participates in the declared `Manifest` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Manifest.run_id`
- **Purpose:** `run_id` participates in the declared `Manifest` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Manifest.core_version`
- **Purpose:** `core_version` participates in the declared `Manifest` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Manifest.git_revision`
- **Purpose:** `git_revision` participates in the declared `Manifest` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Manifest.compiler`
- **Purpose:** `compiler` participates in the declared `Manifest` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Manifest.build_flags`
- **Purpose:** `build_flags` participates in the declared `Manifest` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Manifest.config_digest`
- **Purpose:** `config_digest` participates in the declared `Manifest` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Manifest.seed`
- **Purpose:** `seed` participates in the declared `Manifest` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Manifest.status`
- **Purpose:** `status` participates in the declared `Manifest` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Manifest.artifacts`
- **Purpose:** `artifacts` participates in the declared `Manifest` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Manifest.trace_digest`
- **Purpose:** `trace_digest` participates in the declared `Manifest` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Manifest.duration_ns`
- **Purpose:** `duration_ns` participates in the declared `Manifest` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
### Schema RunConfig
#### `RunConfig.symbol`
- **Purpose:** `symbol` participates in the declared `RunConfig` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `RunConfig.tick_size_minor`
- **Purpose:** `tick_size_minor` participates in the declared `RunConfig` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `RunConfig.duration_ns`
- **Purpose:** `duration_ns` participates in the declared `RunConfig` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `RunConfig.seed`
- **Purpose:** `seed` participates in the declared `RunConfig` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `RunConfig.agent_mix`
- **Purpose:** `agent_mix` participates in the declared `RunConfig` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `RunConfig.starting_cash`
- **Purpose:** `starting_cash` participates in the declared `RunConfig` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `RunConfig.starting_inventory`
- **Purpose:** `starting_inventory` participates in the declared `RunConfig` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `RunConfig.max_events`
- **Purpose:** `max_events` participates in the declared `RunConfig` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `RunConfig.max_orders`
- **Purpose:** `max_orders` participates in the declared `RunConfig` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `RunConfig.scenario`
- **Purpose:** `scenario` participates in the declared `RunConfig` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `RunConfig.latency_model`
- **Purpose:** `latency_model` participates in the declared `RunConfig` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
### Schema RunSummary
#### `RunSummary.run_id`
- **Purpose:** `run_id` participates in the declared `RunSummary` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `RunSummary.trade_count`
- **Purpose:** `trade_count` participates in the declared `RunSummary` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `RunSummary.event_count`
- **Purpose:** `event_count` participates in the declared `RunSummary` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `RunSummary.mean_spread_ticks`
- **Purpose:** `mean_spread_ticks` participates in the declared `RunSummary` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `RunSummary.quoted_depth`
- **Purpose:** `quoted_depth` participates in the declared `RunSummary` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `RunSummary.agent_pnl`
- **Purpose:** `agent_pnl` participates in the declared `RunSummary` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `RunSummary.agent_positions`
- **Purpose:** `agent_positions` participates in the declared `RunSummary` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `RunSummary.trace_digest`
- **Purpose:** `trace_digest` participates in the declared `RunSummary` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `RunSummary.stop_reason`
- **Purpose:** `stop_reason` participates in the declared `RunSummary` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `RunSummary.quality_flags`
- **Purpose:** `quality_flags` participates in the declared `RunSummary` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
### Schema Experiment
#### `Experiment.experiment_id`
- **Purpose:** `experiment_id` participates in the declared `Experiment` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Experiment.scenario_id`
- **Purpose:** `scenario_id` participates in the declared `Experiment` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Experiment.variants`
- **Purpose:** `variants` participates in the declared `Experiment` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Experiment.paired_seeds`
- **Purpose:** `paired_seeds` participates in the declared `Experiment` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Experiment.primary_metric`
- **Purpose:** `primary_metric` participates in the declared `Experiment` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Experiment.secondary_metrics`
- **Purpose:** `secondary_metrics` participates in the declared `Experiment` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Experiment.max_runs`
- **Purpose:** `max_runs` participates in the declared `Experiment` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Experiment.resource_budget`
- **Purpose:** `resource_budget` participates in the declared `Experiment` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `Experiment.status`
- **Purpose:** `status` participates in the declared `Experiment` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
### Schema OrderStatus
#### `OrderStatus.order_id`
- **Purpose:** `order_id` participates in the declared `OrderStatus` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `OrderStatus.accepted`
- **Purpose:** `accepted` participates in the declared `OrderStatus` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `OrderStatus.rejected`
- **Purpose:** `rejected` participates in the declared `OrderStatus` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `OrderStatus.resting`
- **Purpose:** `resting` participates in the declared `OrderStatus` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `OrderStatus.partially_filled`
- **Purpose:** `partially_filled` participates in the declared `OrderStatus` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `OrderStatus.filled`
- **Purpose:** `filled` participates in the declared `OrderStatus` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `OrderStatus.canceled`
- **Purpose:** `canceled` participates in the declared `OrderStatus` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `OrderStatus.remaining_qty`
- **Purpose:** `remaining_qty` participates in the declared `OrderStatus` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `OrderStatus.reason_code`
- **Purpose:** `reason_code` participates in the declared `OrderStatus` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
### Schema AgentObservation
#### `AgentObservation.timestamp_ns`
- **Purpose:** `timestamp_ns` participates in the declared `AgentObservation` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `AgentObservation.last_trade_ticks`
- **Purpose:** `last_trade_ticks` participates in the declared `AgentObservation` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `AgentObservation.best_bid_ticks`
- **Purpose:** `best_bid_ticks` participates in the declared `AgentObservation` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `AgentObservation.best_ask_ticks`
- **Purpose:** `best_ask_ticks` participates in the declared `AgentObservation` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `AgentObservation.depth`
- **Purpose:** `depth` participates in the declared `AgentObservation` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `AgentObservation.own_open_orders`
- **Purpose:** `own_open_orders` participates in the declared `AgentObservation` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `AgentObservation.own_cash`
- **Purpose:** `own_cash` participates in the declared `AgentObservation` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `AgentObservation.own_inventory`
- **Purpose:** `own_inventory` participates in the declared `AgentObservation` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `AgentObservation.public_signal`
- **Purpose:** `public_signal` participates in the declared `AgentObservation` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
### Schema MetricRow
#### `MetricRow.run_id`
- **Purpose:** `run_id` participates in the declared `MetricRow` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `MetricRow.name`
- **Purpose:** `name` participates in the declared `MetricRow` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `MetricRow.value`
- **Purpose:** `value` participates in the declared `MetricRow` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `MetricRow.unit`
- **Purpose:** `unit` participates in the declared `MetricRow` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `MetricRow.method_version`
- **Purpose:** `method_version` participates in the declared `MetricRow` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `MetricRow.sampling_rule`
- **Purpose:** `sampling_rule` participates in the declared `MetricRow` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
#### `MetricRow.quality_flag`
- **Purpose:** `quality_flag` participates in the declared `MetricRow` contract and is never implicitly inferred from unrelated fields.
- **Type:** Use a strongly typed integer, enum, immutable identifier, or documented optional/container according to domain semantics.
- **Validation:** Reject invalid range, unknown enum, forbidden null, or inconsistent state before downstream mutation.
- **Serialization:** Preserve canonical field spelling, integer units, and explicit schema version with deterministic ordering.
- **Replay:** Include this field in hashes when it affects execution semantics; explicitly document exceptions.
- **Consumers:** Document native/Python/API/UI readers and ensure every consumer handles missing/invalid values safely.
- **Tests:** Add valid, invalid, boundary, and round-trip fixtures with expected deterministic results.
