"""Structural models of the native run and experiment config schemas.

These exist to give field-level errors and to describe each parameter (unit, range) to the
terminal. They are not the authority: a config that passes here is still handed to the
core, whose validation decides. Ranges are the static per-field bounds of
``core/src/simulation/scenario.cpp``; rules that relate two fields are left to the core.

The API accepts a narrower range than the core where a value would otherwise exceed what
a browser can hold exactly (2^53 - 1): seeds, agent IDs, and cash balances.
"""

from __future__ import annotations

from typing import Annotated, Any, Literal

from pydantic import BaseModel, ConfigDict, Field, StrictInt, StrictStr

JS_SAFE_INTEGER = 2**53 - 1
MAX_DURATION_NS = 10**15
MAX_HORIZON_NS = 10**18
MAX_TICKS = 10**9
MAX_MILLITICKS = 10**12
MAX_SHARES = 10**6
PPM = 10**6
# Agent order IDs are (agent ID << 32) + n; this keeps them exactly representable.
MAX_API_AGENT_ID = 2**20 - 1
MAX_API_CASH_MINOR = 9 * 10**15

NAME_PATTERN = r"^[A-Za-z0-9_-]+$"


def _field(unit: str, description: str, **bounds: Any) -> Any:
    return Field(description=description, json_schema_extra={"unit": unit}, **bounds)


class Strict(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)


# ---- exchange ---------------------------------------------------------------------------


class ExchangeModel(Strict):
    symbol: StrictInt = _field("id", "Symbol the exchange trades", ge=1, le=JS_SAFE_INTEGER)
    agents: list[Annotated[StrictInt, Field(ge=1, le=MAX_API_AGENT_ID)]] = _field(
        "ids", "Agents registered with the exchange"
    )
    max_order_quantity: StrictInt = _field("shares", "Largest accepted order", ge=1)
    max_active_orders: StrictInt = _field("orders", "Resting order cap", ge=1)
    max_accepted_orders: StrictInt = _field("orders", "Lifetime accepted order cap", ge=1)
    max_trades: StrictInt = _field("trades", "Lifetime trade cap", ge=1)
    max_snapshot_depth: StrictInt = _field("levels", "Deepest snapshot allowed", ge=1)


# ---- agents -----------------------------------------------------------------------------


class WakeupModel(Strict):
    first_ns: StrictInt = _field("ns", "Time of the first wakeup", ge=0, le=MAX_HORIZON_NS)
    interval_ns: StrictInt = _field(
        "ns",
        "Wakeup interval: exact when Fixed, the mean when Exponential",
        ge=1,
        le=MAX_DURATION_NS,
    )
    distribution: Literal["Fixed", "Exponential"] = _field("", "Wakeup schedule")


class LatencyModel(Strict):
    order_entry_ns: StrictInt = _field(
        "ns", "Delay on orders and cancels", ge=0, le=MAX_DURATION_NS
    )
    order_entry_jitter_ns: StrictInt = _field(
        "ns", "Uniform jitter added to order entry", ge=0, le=MAX_DURATION_NS
    )
    report_ns: StrictInt = _field(
        "ns", "Delay on order reports and fills", ge=0, le=MAX_DURATION_NS
    )
    market_data_ns: StrictInt = _field("ns", "Delay on market data", ge=0, le=MAX_DURATION_NS)


class LimitsModel(Strict):
    max_position: StrictInt = _field("shares", "Position limit, long or short", ge=1, le=MAX_SHARES)
    max_order_quantity: StrictInt = _field("shares", "Largest single order", ge=1, le=MAX_SHARES)
    max_open_orders: StrictInt = _field("orders", "Working order cap", ge=1, le=4096)
    max_messages: StrictInt = _field("messages", "Lifetime message budget", ge=1, le=2**32 - 1)


class MarketMakerPolicy(Strict):
    kind: Literal["MarketMaker"]
    half_spread_ticks: StrictInt = _field(
        "ticks", "Distance of the first quote from the reference", ge=1, le=MAX_TICKS
    )
    quote_size: StrictInt = _field("shares", "Size of each quote", ge=1, le=MAX_SHARES)
    levels: StrictInt = _field("levels", "Quotes per side", ge=1, le=32)
    level_spacing_ticks: StrictInt = _field(
        "ticks", "Distance between quote levels", ge=1, le=MAX_TICKS
    )


class InventoryMarketMakerPolicy(Strict):
    kind: Literal["InventoryMarketMaker"]
    half_spread_ticks: StrictInt = _field(
        "ticks", "Distance of the first quote from the reservation price", ge=1, le=MAX_TICKS
    )
    quote_size: StrictInt = _field("shares", "Size of each quote", ge=1, le=MAX_SHARES)
    levels: StrictInt = _field("levels", "Quotes per side", ge=1, le=32)
    level_spacing_ticks: StrictInt = _field(
        "ticks", "Distance between quote levels", ge=1, le=MAX_TICKS
    )
    risk_aversion_ppm: StrictInt = _field(
        "ppm", "Weight of inventory risk in the reservation price", ge=0, le=PPM
    )
    volatility_alpha_ppm: StrictInt = _field(
        "ppm", "EWMA weight of the variance estimate", ge=0, le=PPM
    )
    volatility_floor_milliticks: StrictInt = _field(
        "milliticks", "Floor of the volatility estimate", ge=0, le=10**6
    )
    max_skew_ticks: StrictInt = _field(
        "ticks", "Clamp on the inventory skew and widening", ge=0, le=MAX_TICKS
    )


class MomentumPolicy(Strict):
    kind: Literal["Momentum"]
    short_window: StrictInt = _field("samples", "Short moving-average window", ge=1, le=4095)
    long_window: StrictInt = _field("samples", "Long moving-average window", ge=2, le=4096)
    threshold_milliticks: StrictInt = _field(
        "milliticks", "Signal needed to trade", ge=0, le=MAX_MILLITICKS
    )
    order_size: StrictInt = _field("shares", "Size of each order", ge=1, le=MAX_SHARES)
    slippage_ticks: StrictInt = _field(
        "ticks", "How far through the touch the IOC limit is set", ge=0, le=MAX_TICKS
    )
    cooldown_ns: StrictInt = _field("ns", "Minimum time between orders", ge=0, le=MAX_DURATION_NS)


class ValuePolicy(Strict):
    kind: Literal["Value"]
    estimate_noise_milliticks: StrictInt = _field(
        "milliticks", "Standard deviation of the fundamental estimate", ge=0, le=10**9
    )
    threshold_milliticks: StrictInt = _field(
        "milliticks", "Edge needed to trade", ge=0, le=MAX_MILLITICKS
    )
    order_size: StrictInt = _field("shares", "Size per threshold of edge", ge=1, le=MAX_SHARES)
    slippage_ticks: StrictInt = _field(
        "ticks", "How far through the touch the IOC limit is set", ge=0, le=MAX_TICKS
    )


class NoisePolicy(Strict):
    kind: Literal["Noise"]
    buy_probability_ppm: StrictInt = _field("ppm", "Probability an order is a buy", ge=0, le=PPM)
    market_order_ppm: StrictInt = _field(
        "ppm", "Probability an order is a market order", ge=0, le=PPM
    )
    min_quantity: StrictInt = _field("shares", "Smallest order", ge=1, le=MAX_SHARES)
    max_quantity: StrictInt = _field("shares", "Largest order", ge=1, le=MAX_SHARES)
    limit_offset_ticks: StrictInt = _field(
        "ticks", "Largest distance of a limit order from the reference", ge=0, le=MAX_TICKS
    )
    max_resting_orders: StrictInt = _field(
        "orders", "Resting orders kept before the oldest is canceled", ge=1, le=4095
    )


PolicyModel = Annotated[
    MarketMakerPolicy | InventoryMarketMakerPolicy | MomentumPolicy | ValuePolicy | NoisePolicy,
    Field(discriminator="kind"),
]

POLICY_MODELS: dict[str, type[Strict]] = {
    "MarketMaker": MarketMakerPolicy,
    "InventoryMarketMaker": InventoryMarketMakerPolicy,
    "Momentum": MomentumPolicy,
    "Value": ValuePolicy,
    "Noise": NoisePolicy,
}

POLICY_DESCRIPTIONS: dict[str, str] = {
    "MarketMaker": "Quotes a fixed ladder on both sides of the delivered reference price.",
    "InventoryMarketMaker": (
        "The same ladder around a reservation price that moves against inventory and widens "
        "with estimated variance. A stylized heuristic, not a calibrated model."
    ),
    "Momentum": "Sends an IOC limit in the direction of a short-minus-long moving average.",
    "Value": "Trades toward a noisy estimate of the latent fundamental value.",
    "Noise": "Poisson arrivals of random side, type, size, and offset.",
}


class AgentModel(Strict):
    id: StrictInt = _field(
        "id", "Agent ID, registered with the exchange", ge=1, le=MAX_API_AGENT_ID
    )
    wakeup: WakeupModel
    latency: LatencyModel
    limits: LimitsModel
    policy: PolicyModel


# ---- scenario ---------------------------------------------------------------------------


class FundamentalModel(Strict):
    initial_milliticks: StrictInt = _field(
        "milliticks", "Initial latent value and anchor", ge=1000, le=MAX_MILLITICKS
    )
    update_interval_ns: StrictInt = _field(
        "ns", "Time between steps of the latent value", ge=1, le=MAX_DURATION_NS
    )
    volatility_milliticks: StrictInt = _field(
        "milliticks", "Standard deviation of one step", ge=0, le=MAX_TICKS
    )
    mean_reversion_ppm: StrictInt = _field("ppm", "Pull toward the anchor per step", ge=0, le=PPM)


class SamplingModel(Strict):
    interval_ns: StrictInt = _field(
        "ns", "Time between depth snapshots and accounting samples", ge=1, le=MAX_DURATION_NS
    )
    depth: StrictInt = _field("levels", "Price levels recorded per side", ge=1, le=1000)


class FundamentalJumpShock(Strict):
    kind: Literal["FundamentalJump"]
    at_ns: StrictInt = _field("ns", "Instant of the jump", ge=0, le=MAX_HORIZON_NS)
    delta_milliticks: StrictInt = _field(
        "milliticks",
        "Shift of the latent value and its anchor; nonzero",
        ge=-MAX_MILLITICKS,
        le=MAX_MILLITICKS,
    )


class LiquidityWithdrawalShock(Strict):
    kind: Literal["LiquidityWithdrawal"]
    start_ns: StrictInt = _field("ns", "Start of the interval (inclusive)", ge=0, le=MAX_HORIZON_NS)
    end_ns: StrictInt = _field("ns", "End of the interval (exclusive)", ge=1, le=MAX_HORIZON_NS)
    agents: list[Annotated[StrictInt, Field(ge=1, le=MAX_API_AGENT_ID)]] = _field(
        "ids", "Market makers whose quote size is scaled", min_length=1
    )
    quote_size_ppm: StrictInt = _field(
        "ppm", "Scale applied to quote size; 0 withdraws entirely", ge=0, le=PPM
    )


class VolatilityRegimeShock(Strict):
    kind: Literal["VolatilityRegime"]
    start_ns: StrictInt = _field("ns", "Start of the interval (inclusive)", ge=0, le=MAX_HORIZON_NS)
    end_ns: StrictInt = _field("ns", "End of the interval (exclusive)", ge=1, le=MAX_HORIZON_NS)
    volatility_ppm: StrictInt = _field(
        "ppm", "Scale applied to fundamental step volatility", ge=0, le=100 * PPM
    )


ShockModel = Annotated[
    FundamentalJumpShock | LiquidityWithdrawalShock | VolatilityRegimeShock,
    Field(discriminator="kind"),
]

SHOCK_MODELS: dict[str, type[Strict]] = {
    "FundamentalJump": FundamentalJumpShock,
    "LiquidityWithdrawal": LiquidityWithdrawalShock,
    "VolatilityRegime": VolatilityRegimeShock,
}

SHOCK_DESCRIPTIONS: dict[str, str] = {
    "FundamentalJump": "Shifts the latent value and its anchor at one instant.",
    "LiquidityWithdrawal": "Scales named market makers' quote sizes over [start, end).",
    "VolatilityRegime": "Scales the fundamental's step volatility over [start, end).",
}


class ScenarioModel(Strict):
    reference_price_ticks: StrictInt = _field(
        "ticks", "Public reference price, used before any trade", ge=1, le=MAX_TICKS
    )
    fundamental: FundamentalModel
    sampling: SamplingModel | None
    agents: list[AgentModel] = Field(max_length=1024)
    shocks: list[ShockModel] = Field(max_length=1024)


# ---- accounting -------------------------------------------------------------------------


class FeesModel(Strict):
    maker_ppm: StrictInt = _field(
        "ppm", "Maker fee on notional; negative is a rebate", ge=-PPM, le=PPM
    )
    taker_ppm: StrictInt = _field(
        "ppm", "Taker fee on notional; negative is a rebate", ge=-PPM, le=PPM
    )


class AccountModel(Strict):
    agent_id: StrictInt = _field("id", "Agent this exception applies to", ge=1, le=MAX_API_AGENT_ID)
    initial_cash_minor: StrictInt = _field(
        "minor", "Opening cash", ge=-MAX_API_CASH_MINOR, le=MAX_API_CASH_MINOR
    )
    credit_limit_minor: StrictInt | None = _field(
        "minor",
        "Credit allowed before a buy intent is dropped; null is unconstrained",
        ge=0,
        le=MAX_API_CASH_MINOR,
    )


class AccountingModel(Strict):
    fees: FeesModel
    default_initial_cash_minor: StrictInt = _field(
        "minor", "Opening cash of every other agent", ge=-MAX_API_CASH_MINOR, le=MAX_API_CASH_MINOR
    )
    accounts: list[AccountModel]


# ---- run config -------------------------------------------------------------------------


class ScheduledCommandModel(Strict):
    timestamp_ns: StrictInt = Field(ge=0, le=MAX_HORIZON_NS)
    # Scripted exchange commands are validated by the core; the terminal does not edit them.
    command: dict[str, Any]


class RunConfigBase(Strict):
    run_id: StrictStr = Field(
        min_length=1, max_length=128, pattern=NAME_PATTERN, description="Run name"
    )
    seed: StrictInt = _field("seed", "Seed of every random stream", ge=0, le=JS_SAFE_INTEGER)
    tick_size_minor: StrictInt = _field(
        "minor/tick", "Minor currency units per tick", ge=1, le=10**9
    )
    exchange: ExchangeModel
    horizon_ns: StrictInt = _field("ns", "Simulated horizon (inclusive)", ge=0, le=MAX_HORIZON_NS)
    max_events: StrictInt = _field("events", "Dispatched event cap", ge=1)
    max_queue: StrictInt = _field("events", "Scheduler queue cap", ge=1)
    max_trace_bytes: StrictInt = _field("bytes", "Byte budget of each output stream", ge=1)
    commands: list[ScheduledCommandModel]


class ScriptedRunConfig(RunConfigBase):
    schema_version: Literal[1]


class ScenarioRunConfig(RunConfigBase):
    schema_version: Literal[2]
    scenario: ScenarioModel


class AccountingRunConfig(RunConfigBase):
    schema_version: Literal[3]
    scenario: ScenarioModel | None
    accounting: AccountingModel


RunConfigModel = Annotated[
    ScriptedRunConfig | ScenarioRunConfig | AccountingRunConfig,
    Field(discriminator="schema_version"),
]


# ---- experiment config ------------------------------------------------------------------


class SeedRangeModel(Strict):
    first: StrictInt = Field(ge=0, le=JS_SAFE_INTEGER)
    count: StrictInt = Field(ge=1, le=100_000)


class PolicyOverrideModel(Strict):
    agent_id: StrictInt = Field(ge=1, le=MAX_API_AGENT_ID)
    policy: PolicyModel


class VariantModel(Strict):
    name: StrictStr = Field(min_length=1, max_length=48, pattern=NAME_PATTERN)
    policy_overrides: list[PolicyOverrideModel]


class ComparisonModel(Strict):
    baseline: StrictStr
    candidate: StrictStr
    subject_agent_id: StrictInt = Field(ge=1, le=MAX_API_AGENT_ID)


class BootstrapModel(Strict):
    resamples: StrictInt = _field(
        "resamples", "Bootstrap resamples; 0 turns the interval off", ge=0, le=1_000_000
    )
    seed: StrictInt = _field("seed", "Seed of the bootstrap stream", ge=0, le=JS_SAFE_INTEGER)


class ExperimentLimitsModel(Strict):
    max_runs: StrictInt = _field("runs", "Bound on seeds times variants", ge=1, le=100_000)
    max_output_bytes: StrictInt = _field(
        "bytes", "Budget for run directories when artifacts is All", ge=0
    )


class ExperimentConfigModel(Strict):
    schema_version: Literal[1]
    experiment_id: StrictStr = Field(min_length=1, max_length=48, pattern=NAME_PATTERN)
    # The CLI also accepts a file path here. The API never resolves a path from a request,
    # so the base config must be embedded.
    base_config: RunConfigModel
    horizon_ns: StrictInt | None = Field(ge=0, le=MAX_HORIZON_NS)
    seeds: list[Annotated[StrictInt, Field(ge=0, le=JS_SAFE_INTEGER)]] | SeedRangeModel
    variants: list[VariantModel] = Field(min_length=1)
    comparison: ComparisonModel | None
    bootstrap: BootstrapModel
    artifacts: Literal["None", "All"]
    limits: ExperimentLimitsModel
