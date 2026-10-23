"""Constrained, versioned research objects. Native experiment JSON remains authoritative."""

from __future__ import annotations

from enum import StrEnum
from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field


class Intent(StrEnum):
    COMPARE_STRATEGIES = "compare_strategies"
    MEASURE_SHOCK = "measure_shock"
    ANALYZE_EXPERIMENT = "analyze_experiment"
    EXPLAIN_METRIC = "explain_metric"
    INSPECT_SEED = "inspect_seed"
    UNSUPPORTED = "unsupported"


# Mirrors core/src/experiment/experiment.cpp kMetrics. A test checks the native
# golden experiment artifact so this model-facing catalogue cannot drift silently.
NATIVE_EXPERIMENT_METRICS = (
    "total_pnl",
    "realized_pnl",
    "unrealized_pnl",
    "fees_paid",
    "max_drawdown",
    "maker_spread_capture",
    "final_abs_position",
    "max_abs_position",
    "mean_abs_position",
    "traded_quantity",
    "fill_rate",
    "trade_count",
    "volume",
    "mean_spread",
    "mean_relative_spread",
    "mean_bid_depth",
    "mean_ask_depth",
    "midpoint_return_volatility",
    "two_sided_time",
)


class Proposal(BaseModel):
    model_config = ConfigDict(extra="forbid")
    intent: Intent
    scenario_key: str | None = None
    seed_count: int | None = Field(default=None, ge=1)
    subject_agent_id: int | None = Field(default=None, ge=1)
    experiment_id: str | None = None
    primary_metric: str | None = Field(
        default=None, json_schema_extra={"enum": list(NATIVE_EXPERIMENT_METRICS)}
    )
    secondary_metrics: list[str] = Field(
        default_factory=list,
        max_length=8,
        json_schema_extra={"items": {"type": "string", "enum": list(NATIVE_EXPERIMENT_METRICS)}},
    )
    analysis_topics: list[str] = Field(default_factory=list, max_length=8)
    hypothesis: str = Field(max_length=500)
    limitations: list[str] = Field(default_factory=list, max_length=8)
    clarification: str | None = Field(default=None, max_length=500)


class PlanShock(BaseModel):
    """The scenario shock a `measure_shock` plan removes from its `no_shock` variant."""

    model_config = ConfigDict(extra="forbid")
    index: int
    kind: str
    start_ns: int
    end_ns: int | None = None
    agents: list[int] = Field(default_factory=list)


class ResearchPlan(BaseModel):
    model_config = ConfigDict(extra="forbid")
    version: Literal[1] = 1
    question: str
    intent: Intent
    hypothesis: str
    scenario_key: str
    scenario_digest: str
    subject_agent_id: int
    baseline: Literal["baseline_maker", "no_shock"] = "baseline_maker"
    candidate: Literal["inventory_maker", "shock"] = "inventory_maker"
    shock: PlanShock | None = None
    seed_count: int
    primary_metric: str
    secondary_metrics: list[str]
    analysis_topics: list[str] = Field(default_factory=list)
    methodology: str
    limitations: list[str]
    total_runs: int
    config_digest: str
    experiment_config: dict[str, Any]
    approval_required: Literal[True] = True


class Claim(BaseModel):
    model_config = ConfigDict(extra="forbid")
    metric: str
    scope: str
    unit: str
    pairs: int
    baseline_mean_milli: int | None
    candidate_mean_milli: int | None
    difference_mean_milli: int | None
    difference_t_interval_milli: tuple[int, int] | None
    difference_bootstrap_interval_milli: tuple[int, int] | None
    evidence_ref: str


class ReportInterpretation(BaseModel):
    model_config = ConfigDict(extra="forbid")
    interpretation: str = Field(max_length=3000)
    limitations: list[str] = Field(max_length=6)
    follow_up: list[str] = Field(max_length=3)


class FollowupAnswer(BaseModel):
    model_config = ConfigDict(extra="forbid")
    answer: str = Field(max_length=2500)
    evidence_refs: list[str] = Field(max_length=10)


class ResearchReport(BaseModel):
    version: Literal[1]
    question: str
    hypothesis: str
    plan: ResearchPlan | None
    experiment_id: str
    engine_version: str
    config_digest: str
    results_digest: str
    baseline: str
    candidate: str
    pairs_completed: int
    claims: list[Claim]
    interpretation: str
    limitations: list[str]
    follow_up: list[str]
    replay_status: str


class InvestigationView(BaseModel):
    id: str
    question: str
    status: str
    created_at: str
    updated_at: str
    provider: str | None = None
    model: str | None = None
    plan: ResearchPlan | None = None
    experiment_id: str | None = None
    report: ResearchReport | None = None
    error_code: str | None = None
    error_message: str | None = None
    parent_id: str | None = None
    experiment_status: str | None = None
    progress: dict[str, Any] | None = None
    events: list[dict[str, Any]] = Field(default_factory=list)
    messages: list[dict[str, Any]] = Field(default_factory=list)


class InvestigationList(BaseModel):
    total: int
    offset: int
    investigations: list[InvestigationView]


class InvestigationStatus(BaseModel):
    id: str
    status: str
    experiment_id: str | None
    experiment_status: str | None
    progress: dict[str, Any] | None
    error_code: str | None
    error_message: str | None


class ToolEvents(BaseModel):
    events: list[dict[str, Any]]


class SeedEvidence(BaseModel):
    seed: int
    experiment_id: str
    evidence_ref: str
    metrics: dict[str, dict[str, str | int | None]]
