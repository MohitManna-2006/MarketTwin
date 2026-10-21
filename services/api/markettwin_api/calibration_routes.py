"""Trusted-ID HTTP surface for Phase 5B calibrations (ADR 0011)."""

from __future__ import annotations

from typing import Annotated, Any, Literal

from fastapi import APIRouter, Depends, Query, Request
from pydantic import BaseModel, Field

from markettwin_api.calibration_service import CalibrationResearch

router = APIRouter(prefix="/api/v1/calibrations", tags=["Calibration"])


class CalibrationRequest(BaseModel):
    spec: str = Field(pattern=r"^[a-z0-9_]{1,64}$")
    session_ids: list[str] = Field(min_length=1, max_length=60)


class CalibrationView(BaseModel):
    id: str
    spec_key: str
    spec_digest: str
    title: str
    symbol: str
    feed: str
    observation_kind: Literal["OBSERVED IEX DATA", "FABRICATED TEST DATA"]
    status: str
    created_at: str
    started_at: str | None
    finished_at: str | None
    sessions: list[dict[str, Any]]
    progress: dict[str, Any] | None
    headline: dict[str, Any] | None
    error_code: str | None
    error_message: str | None


class CalibrationListView(BaseModel):
    calibrations: list[CalibrationView]
    offset: int
    count: int


def get_calibration(request: Request) -> CalibrationResearch:
    calibration: CalibrationResearch = request.app.state.calibration
    return calibration


Calibration = Annotated[CalibrationResearch, Depends(get_calibration)]


@router.get("/specs")
def specs(calibration: Calibration) -> Any:
    return calibration.specs()


@router.post("/fixture-sessions", status_code=201)
def fixture_sessions(calibration: Calibration) -> Any:
    return calibration.fixture_sessions()


@router.post("", status_code=202, response_model=CalibrationView)
def create(body: CalibrationRequest, calibration: Calibration) -> Any:
    return calibration.create(body.spec, body.session_ids)


@router.get("", response_model=CalibrationListView)
def calibrations(
    calibration: Calibration,
    offset: Annotated[int, Query(ge=0)] = 0,
    limit: Annotated[int, Query(ge=1, le=100)] = 50,
) -> Any:
    return calibration.list(limit, offset)


@router.get("/{calibration_id}", response_model=CalibrationView)
def status(calibration_id: str, calibration: Calibration) -> Any:
    return calibration.require(calibration_id)


@router.get("/{calibration_id}/report")
def report(calibration_id: str, calibration: Calibration) -> Any:
    return calibration.report(calibration_id)


@router.get("/{calibration_id}/selected-config")
def selected_config(calibration_id: str, calibration: Calibration) -> Any:
    return calibration.selected_config(calibration_id)


@router.get("/{calibration_id}/candidates")
def candidates(
    calibration_id: str,
    calibration: Calibration,
    stage: Literal["stage1", "stage2", "test"] = "stage1",
    offset: Annotated[int, Query(ge=0)] = 0,
    limit: Annotated[int, Query(ge=1, le=100)] = 20,
) -> Any:
    return calibration.candidates(calibration_id, stage, offset, limit)


@router.post("/{calibration_id}/verify")
def verify(calibration_id: str, calibration: Calibration) -> Any:
    return calibration.verify(calibration_id)
