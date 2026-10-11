"""Request and response models of the HTTP API.

Units are part of every name: ``_ns`` is elapsed simulation nanoseconds, ``_ticks`` and
``_milliticks`` are prices, ``_twice_ticks`` is a price doubled so a half-tick midpoint is
an integer, ``_minor`` is money in minor currency units, ``_milli``, ``_ppm``, and ``_ppb``
are fixed-point scales. ``created_at`` and the other ``_at`` fields are wall-clock UTC and
``_ms`` fields are wall-clock processing time; neither is simulation time.
"""

from __future__ import annotations

from typing import Any, Literal

from pydantic import BaseModel, Field

Json = dict[str, Any]

RunStatus = Literal["pending", "running", "completed", "failed"]
ReplayStatus = Literal[
    "not_checked", "running", "verified", "mismatch", "unsupported_version", "error"
]
ExperimentStatus = Literal[
    "pending", "running", "completed", "partially_completed", "failed", "canceled"
]


class ErrorBody(BaseModel):
    code: str
    message: str
    details: Any = None


class ErrorResponse(BaseModel):
    error: ErrorBody


# ---- service ------------------------------------------------------------------------------


class Health(BaseModel):
    status: Literal["ok"]
    api_version: str
    engine_version: str
    git_revision: str
    compiler: str
    build_type: str
    workspace: str
    schemas: dict[str, int]
    active_jobs: int
    max_concurrent_jobs: int
    interrupted_at_startup: int
    limits: dict[str, int]
    counts: dict[str, int]


# ---- scenarios ----------------------------------------------------------------------------


class ScenarioSummary(BaseModel):
    key: str
    title: str
    kind: Literal["scenario", "scripted"]
    description: str
    schema_version: int
    seed: int
    horizon_ns: int
    tick_size_minor: int
    agent_count: int
    policies: dict[str, int]
    shocks: list[str]
    has_accounting: bool
    config_digest: str


class ScenarioDetail(ScenarioSummary):
    config: Json


class FieldSpec(BaseModel):
    name: str
    unit: str
    description: str
    minimum: int | None
    maximum: int | None
    options: list[str] | None
    nullable: bool


class PolicySpec(BaseModel):
    kind: str
    description: str
    fields: list[FieldSpec]
    template: Json | None


class ShockSpec(BaseModel):
    kind: str
    description: str
    fields: list[FieldSpec]


class FieldCatalogue(BaseModel):
    run: list[FieldSpec]
    scenario: list[FieldSpec]
    fundamental: list[FieldSpec]
    sampling: list[FieldSpec]
    wakeup: list[FieldSpec]
    latency: list[FieldSpec]
    limits: list[FieldSpec]
    fees: list[FieldSpec]
    accounting: list[FieldSpec]
    account: list[FieldSpec]
    policies: list[PolicySpec]
    shocks: list[ShockSpec]


# ---- validation ---------------------------------------------------------------------------


class ConfigError(BaseModel):
    source: Literal["schema", "api", "engine"]
    code: str
    path: str = Field(description="Dotted path into the submitted config; empty when unknown")
    message: str


class RunComplexity(BaseModel):
    horizon_ns: int
    agents: int
    scheduled_wakeups: int | None
    wakeups_exact: bool | None
    fundamental_updates: int | None
    samples: int | None
    sampling_depth: int | None
    shocks: int
    scripted_commands: int
    max_events: int
    max_trace_bytes: int


class ValidateRunRequest(BaseModel):
    config: Any


class RunValidation(BaseModel):
    valid: bool
    errors: list[ConfigError]
    canonical_config: Json | None
    config_digest: str | None
    complexity: RunComplexity | None


class ExperimentDesign(BaseModel):
    seeds: int
    variants: int
    total_runs: int
    pairing: str
    comparison: Json | None
    artifacts: str
    per_run: RunComplexity


class ExperimentValidation(BaseModel):
    valid: bool
    errors: list[ConfigError]
    canonical_config: Json | None
    design: ExperimentDesign | None


# ---- runs ---------------------------------------------------------------------------------


class CreateRunRequest(BaseModel):
    config: Any
    scenario_key: str | None = None


class RunError(BaseModel):
    code: str
    message: str
    sequence: int | None


class CellProvenance(BaseModel):
    experiment_id: str
    variant: str
    seed: int
    expected: dict[str, str | None]
    digests_match: bool | None
    mismatched: list[str]


class RunSummary(BaseModel):
    id: str
    name: str
    scenario_key: str | None
    seed: int
    horizon_ns: int
    status: RunStatus
    replay_status: ReplayStatus
    created_at: str
    started_at: str | None
    finished_at: str | None
    processing_ms: int | None
    engine_version: str
    config_digest: str
    final_time_ns: int | None
    trade_count: int | None
    event_count: int | None
    stop_reason: str | None
    source_kind: Literal["user", "experiment_cell"]
    experiment_id: str | None
    variant: str | None
    error: RunError | None


class RunList(BaseModel):
    total: int
    offset: int
    runs: list[RunSummary]


class AgentView(BaseModel):
    id: int
    policy: str | None
    config: Json | None = Field(
        description="The agent's scenario config; null for a scripted agent"
    )
    initial_cash_minor: int
    credit_limit_minor: int | None
    report: Json | None = Field(description="The agent's entry in metrics.json, verbatim")


class ShockTransition(BaseModel):
    phase: str
    timestamp_ns: int
    sequence: int


class ShockView(BaseModel):
    index: int
    kind: str
    definition: Json
    transitions: list[ShockTransition]


class ArtifactFile(BaseModel):
    name: str
    bytes: int | None
    sha256: str | None


class ReplayView(BaseModel):
    status: ReplayStatus
    checked_at: str | None
    processing_ms: int | None
    verified: bool | None
    code: str | None
    message: str | None
    sequence: int | None
    expected: Any = None
    actual: Any = None
    expected_raw: str | None
    actual_raw: str | None
    previous_command: Any = None
    previous_output: Any = None


class RunAvailability(BaseModel):
    artifacts: bool
    samples: bool
    books: bool
    fundamental: bool
    agents: bool
    trades: bool
    index_error: RunError | None


class RunDetail(RunSummary):
    indexing_ms: int | None
    config: Json
    summary: Json | None
    manifest: Json | None
    metrics: Json | None
    units: Json | None
    agents: list[AgentView]
    shocks: list[ShockView]
    files: list[ArtifactFile]
    sample_count: int
    sampling: Json | None
    availability: RunAvailability
    replay: ReplayView
    cell: CellProvenance | None


# ---- telemetry ----------------------------------------------------------------------------

Column = list[int | None]


class SampleTimes(BaseModel):
    count: int
    timestamp_ns: list[int]


class Series(BaseModel):
    sample_count: int
    decimated: bool
    decimation: str | None
    index: list[int] = Field(description="Sample index of every plotted point")
    derived: list[str] = Field(description="Columns computed from recorded ones, not recorded")
    columns: dict[str, Column]


class MarketSeries(Series):
    mark_sources: list[str]


class AgentSeries(Series):
    agent_id: int


class BookSnapshot(BaseModel):
    timestamp_ns: int
    sequence: int
    bids: list[list[int]] = Field(description="[price_ticks, quantity, order_count], best first")
    asks: list[list[int]]


class BookAt(BookSnapshot):
    index: int


class SampleWindow(BaseModel):
    start: int
    count: int
    sample_count: int
    mark_sources: list[str]
    columns: dict[str, Column]
    books: list[BookSnapshot | None]
    agents: dict[str, dict[str, Column]]


class TradeRow(BaseModel):
    timestamp_ns: int
    sequence: int
    trade_id: int
    price_ticks: int
    quantity: int
    maker_agent_id: int
    taker_agent_id: int
    aggressor_side: Literal["Buy", "Sell"]
    buy_order_id: int
    sell_order_id: int


class TradePage(BaseModel):
    total: int
    offset: int
    rows: list[TradeRow]


class LedgerRow(BaseModel):
    timestamp_ns: int
    sequence: int
    trade_id: int
    side: Literal["Buy", "Sell"]
    role: Literal["Maker", "Taker"] | None
    quantity: int
    price_ticks: int
    notional_minor: int
    fee_minor: int
    cash_minor: int = Field(description="Cash after this trade")
    position: int = Field(description="Position after this trade")
    cost_basis_minor: int = Field(description="Cost basis after this trade")
    realized_pnl_minor: int = Field(description="Cumulative realized P&L after this trade")
    counterparty_agent_id: int


class LedgerPage(BaseModel):
    total: int
    offset: int
    rows: list[LedgerRow]


class EventRow(BaseModel):
    channel: Literal["commands", "execution", "accounting"]
    sequence: int
    timestamp_ns: int
    kind: str
    actor_id: int | None
    order_id: int | None
    causation_id: int | None
    scheduler_sequence: int | None
    price_ticks: int | None
    quantity: int | None
    side: str | None
    state: str | None
    reason: str | None
    payload: Json
    payload_digest: str | None


class EventPage(BaseModel):
    stream: Literal["market", "accounting"]
    total: int
    offset: int
    anchor: int | None = Field(
        description="Offset of the latest matching record at or before the requested point"
    )
    kinds: list[str]
    rows: list[EventRow]


# ---- experiments --------------------------------------------------------------------------


class CreateExperimentRequest(BaseModel):
    config: Any
    scenario_key: str | None = None


class ValidateExperimentRequest(BaseModel):
    config: Any


class ExperimentProgress(BaseModel):
    total: int
    completed: int
    failed: int
    current_run_id: str | None
    cancel_requested: bool


class ExperimentSummary(BaseModel):
    id: str
    name: str
    scenario_key: str | None
    status: ExperimentStatus
    created_at: str
    started_at: str | None
    finished_at: str | None
    processing_ms: int | None
    engine_version: str
    seed_count: int
    variants: list[str]
    comparison: Json | None
    total_runs: int
    completed_runs: int
    failed_runs: int
    pairs_completed: int | None
    config_digest: str | None
    results_digest: str | None
    headline: Json | None
    error: RunError | None
    progress: ExperimentProgress | None


class ExperimentList(BaseModel):
    total: int
    offset: int
    experiments: list[ExperimentSummary]


class CellRun(BaseModel):
    run_id: str
    variant: str
    seed: int
    status: RunStatus
    digests_match: bool | None


class ExperimentDetail(ExperimentSummary):
    config: Json
    cell_runs: list[CellRun]


class ExperimentResults(BaseModel):
    document: Json = Field(description="experiment.json as the core wrote it")


class PairedValues(BaseModel):
    document: Json = Field(description="Per-seed values behind every comparison, from the core")


class MaterializeCellRequest(BaseModel):
    variant: str
    seed: int = Field(ge=0)
