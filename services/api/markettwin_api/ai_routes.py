"""HTTP control surface for supervised AI investigations."""

from __future__ import annotations

from typing import Annotated, Any

from fastapi import APIRouter, Depends, Query, Request
from fastapi.responses import PlainTextResponse
from pydantic import BaseModel, Field

from markettwin_api import ai_models
from markettwin_api.ai_service import AIResearch
from markettwin_api.service import ServiceError

router = APIRouter(prefix="/api/v1/ai", tags=["AI research"])


class CreateInvestigation(BaseModel):
    question: str = Field(min_length=10, max_length=2000)
    parent_id: str | None = Field(default=None, max_length=32)


class ApprovePlan(BaseModel):
    config_digest: str = Field(pattern=r"^[a-f0-9]{64}$")


class Message(BaseModel):
    content: str = Field(min_length=2, max_length=2000)


def get_ai(request: Request) -> AIResearch:
    ai: AIResearch = request.app.state.ai
    return ai


AI = Annotated[AIResearch, Depends(get_ai)]


@router.post("/investigations", status_code=201, response_model=ai_models.InvestigationView)
def create(body: CreateInvestigation, ai: AI) -> Any:
    return ai.create(body.question, body.parent_id)


@router.get("/investigations", response_model=ai_models.InvestigationList)
def list_investigations(
    ai: AI, offset: Annotated[int, Query(ge=0)] = 0, limit: Annotated[int, Query(ge=1, le=100)] = 50
) -> Any:
    return {
        "total": ai.registry.count_investigations(),
        "offset": offset,
        "investigations": ai.registry.investigations(limit, offset),
    }


@router.get("/investigations/{investigation_id}", response_model=ai_models.InvestigationView)
def get(investigation_id: str, ai: AI) -> Any:
    return ai.view(investigation_id)


@router.post("/investigations/{investigation_id}/plan", response_model=ai_models.InvestigationView)
def plan(investigation_id: str, ai: AI) -> Any:
    return ai.plan(investigation_id)


@router.post(
    "/investigations/{investigation_id}/approve", response_model=ai_models.InvestigationView
)
def approve(investigation_id: str, body: ApprovePlan, ai: AI) -> Any:
    return ai.approve(investigation_id, body.config_digest)


@router.post(
    "/investigations/{investigation_id}/reject", response_model=ai_models.InvestigationView
)
def reject(investigation_id: str, ai: AI) -> Any:
    return ai.reject(investigation_id)


@router.post(
    "/investigations/{investigation_id}/cancel", response_model=ai_models.InvestigationView
)
def cancel(investigation_id: str, ai: AI) -> Any:
    return ai.cancel(investigation_id)


@router.get(
    "/investigations/{investigation_id}/status", response_model=ai_models.InvestigationStatus
)
def status(investigation_id: str, ai: AI) -> Any:
    row = ai.view(investigation_id)
    return {
        "id": row["id"],
        "status": row["status"],
        "experiment_id": row["experiment_id"],
        "experiment_status": row["experiment_status"],
        "progress": row["progress"],
        "error_code": row["error_code"],
        "error_message": row["error_message"],
    }


@router.get("/investigations/{investigation_id}/report", response_model=ai_models.ResearchReport)
def report(investigation_id: str, ai: AI) -> Any:
    row = ai.require(investigation_id)
    if row["report"] is None:
        raise ServiceError(409, "ReportNotReady", "this investigation has no report yet")
    return row["report"]


@router.get("/investigations/{investigation_id}/report/markdown", response_class=PlainTextResponse)
def report_markdown(investigation_id: str, ai: AI) -> str:
    return ai.markdown(investigation_id)


@router.get("/investigations/{investigation_id}/tool-events", response_model=ai_models.ToolEvents)
def events(investigation_id: str, ai: AI) -> Any:
    ai.require(investigation_id)
    return {"events": ai.registry.ai_events(investigation_id)}


@router.get(
    "/investigations/{investigation_id}/seeds/{seed}", response_model=ai_models.SeedEvidence
)
def seed(investigation_id: str, seed: int, ai: AI) -> Any:
    return ai.seed(investigation_id, seed)


@router.post(
    "/investigations/{investigation_id}/messages", response_model=ai_models.InvestigationView
)
def message(investigation_id: str, body: Message, ai: AI) -> Any:
    return ai.message(investigation_id, body.content)
