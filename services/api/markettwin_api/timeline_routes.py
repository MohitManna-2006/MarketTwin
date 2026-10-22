"""Trusted-ID HTTP surface for timeline forks and the AI desk (ADR 0012)."""

from __future__ import annotations

from typing import Annotated, Any, Literal

from fastapi import APIRouter, Depends, Request
from pydantic import BaseModel, Field

from markettwin_api.config_models import (
    MAX_DURATION_NS,
    MAX_HORIZON_NS,
    DirectiveModel,
    DirectiveOverrideModel,
    PolicyOverrideModel,
    ShockModel,
)
from markettwin_api.timeline_service import Timelines
from markettwin_api.views import experiment_summary, run_summary

router = APIRouter(prefix="/api/v1", tags=["Timelines"])


class ForkRequest(BaseModel):
    """One change to a finished run from `at_ns` on. Every other field may be left out."""

    title: str | None = Field(default=None, max_length=120)
    at_ns: int = Field(ge=0, le=MAX_HORIZON_NS)
    add_shocks: list[ShockModel] = Field(default_factory=list, max_length=64)
    remove_shocks: list[Annotated[int, Field(ge=0, le=1023)]] = Field(
        default_factory=list, max_length=64
    )
    policy_overrides: list[PolicyOverrideModel] = Field(default_factory=list, max_length=64)
    horizon_ns: int | None = Field(default=None, ge=1, le=MAX_HORIZON_NS)
    decisions: Literal["hold", "parent", "live"] = "hold"
    directive_overrides: list[DirectiveOverrideModel] = Field(default_factory=list, max_length=64)

    def stage(self) -> dict[str, Any]:
        return self.model_dump(mode="json", exclude={"title"})


class DeskRequest(BaseModel):
    scenario: str = Field(pattern=r"^[a-z0-9_]{1,64}$")
    agent_id: int = Field(ge=1, le=2**20 - 1)
    seed: int = Field(default=42, ge=0, le=2**53 - 1)
    horizon_ns: int | None = Field(default=None, ge=1, le=MAX_HORIZON_NS)
    mandate: str | None = Field(default=None, max_length=2000, pattern=r"^[ -~\n]*$")
    decision_interval_ns: int = Field(default=60_000_000_000, ge=1_000_000, le=MAX_DURATION_NS)
    max_half_spread_ticks: int = Field(default=20, ge=1, le=10**9)
    max_skew_ticks: int = Field(default=10, ge=0, le=10**9)
    initial: DirectiveModel | None = None
    source: Literal["model", "fixture"] = "fixture"
    approved_calls: int | None = Field(default=None, ge=0)


class RepeatRequest(BaseModel):
    seeds: int = Field(default=20, ge=2, le=1000)
    first_seed: int = Field(default=1, ge=0, le=2**53 - 1)


def get_timelines(request: Request) -> Timelines:
    timelines: Timelines = request.app.state.timelines
    return timelines


TimelinesDep = Annotated[Timelines, Depends(get_timelines)]


@router.post("/runs/{run_id}/forks", status_code=202)
def create_fork(run_id: str, body: ForkRequest, timelines: TimelinesDep) -> Any:
    return run_summary(timelines.create_fork(run_id, body.stage(), body.title))


@router.get("/runs/{run_id}/timeline")
def timeline(run_id: str, timelines: TimelinesDep) -> Any:
    return timelines.timeline(run_id)


@router.get("/runs/{run_id}/divergence")
def divergence(run_id: str, timelines: TimelinesDep) -> Any:
    return timelines.divergence(run_id)


@router.get("/runs/{run_id}/decisions")
def decisions(run_id: str, timelines: TimelinesDep) -> Any:
    return timelines.decisions(run_id)


@router.get("/runs/{run_id}/decision-ledger")
def ledger(run_id: str, timelines: TimelinesDep) -> Any:
    return timelines.ledger(run_id)


@router.post("/runs/{run_id}/decision-ledger", status_code=202)
def start_ledger(run_id: str, timelines: TimelinesDep) -> Any:
    return timelines.start_ledger(run_id)


@router.post("/runs/{run_id}/repeat", status_code=202)
def repeat(run_id: str, body: RepeatRequest, timelines: TimelinesDep) -> Any:
    return experiment_summary(
        timelines.research, timelines.repeat(run_id, body.seeds, body.first_seed)
    )


@router.post("/desks/estimate")
def estimate(body: DeskRequest, timelines: TimelinesDep) -> Any:
    return timelines.estimate(body.model_dump(mode="json"))


@router.post("/desks", status_code=202)
def create_desk(body: DeskRequest, timelines: TimelinesDep) -> Any:
    return run_summary(timelines.create_desk(body.model_dump(mode="json")))
