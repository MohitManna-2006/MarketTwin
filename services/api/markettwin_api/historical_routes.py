"""Trusted-ID HTTP surface for observed historical market sessions."""

from __future__ import annotations

from typing import Annotated, Any, Literal

from fastapi import APIRouter, Depends, Query, Request
from pydantic import BaseModel, Field

from markettwin_api.historical_service import HistoricalResearch

router = APIRouter(prefix="/api/v1/historical", tags=["Historical observations"])


class ImportRequest(BaseModel):
    provider: Literal["local_file", "alpaca"]
    symbol: str = Field(pattern=r"^[A-Z][A-Z.]{0,11}$")
    feed: str = Field(max_length=32)
    start: str | None = Field(default=None, max_length=35)
    end: str | None = Field(default=None, max_length=35)
    confirm_access: bool = False


class ProviderView(BaseModel):
    id: str
    configured: bool
    feeds: list[str]
    coverage: str
    cost: str


class ProvidersView(BaseModel):
    providers: list[ProviderView]


class DatasetView(BaseModel):
    id: str
    symbol: str
    feed: str
    description: str


class DatasetsView(BaseModel):
    datasets: list[DatasetView]


class ImportView(BaseModel):
    id: str
    provider: str
    feed: str
    symbol: str
    start_time: str | None
    end_time: str | None
    status: str
    created_at: str
    started_at: str | None
    finished_at: str | None
    manifest: dict[str, Any] | None
    error_code: str | None
    error_message: str | None


class ImportListView(BaseModel):
    imports: list[ImportView]
    offset: int
    count: int


class SessionListView(BaseModel):
    sessions: list[ImportView]
    offset: int
    count: int


class SessionView(ImportView):
    quality: dict[str, Any]
    metrics: dict[str, Any]
    observation_kind: Literal["REAL HISTORICAL DATA", "FABRICATED TEST DATA"]


class RecordPageView(BaseModel):
    session_id: str
    offset: int
    records: list[dict[str, Any]]
    next_offset: int | None


def get_historical(request: Request) -> HistoricalResearch:
    historical: HistoricalResearch = request.app.state.historical
    return historical


Historical = Annotated[HistoricalResearch, Depends(get_historical)]


@router.get("/providers", response_model=ProvidersView)
def providers(historical: Historical) -> Any:
    return historical.providers()


@router.get("/datasets", response_model=DatasetsView)
def datasets(historical: Historical) -> Any:
    return historical.datasets()


@router.post("/imports", status_code=202, response_model=ImportView)
def create_import(body: ImportRequest, historical: Historical) -> Any:
    return historical.create(
        body.provider, body.symbol, body.feed, body.start, body.end, body.confirm_access
    )


@router.get("/imports", response_model=ImportListView)
def imports(
    historical: Historical,
    offset: Annotated[int, Query(ge=0)] = 0,
    limit: Annotated[int, Query(ge=1, le=100)] = 50,
) -> Any:
    return historical.list(limit, offset)


@router.get("/imports/{import_id}", response_model=ImportView)
def import_status(import_id: str, historical: Historical) -> Any:
    return historical.require(import_id)


@router.get("/sessions", response_model=SessionListView)
def sessions(
    historical: Historical,
    offset: Annotated[int, Query(ge=0)] = 0,
    limit: Annotated[int, Query(ge=1, le=100)] = 50,
) -> Any:
    rows = historical.list(limit, offset)
    rows["sessions"] = [row for row in rows.pop("imports") if row["status"] == "completed"]
    rows["count"] = len(rows["sessions"])
    return rows


@router.get("/sessions/{session_id}", response_model=SessionView)
def session(session_id: str, historical: Historical) -> Any:
    return historical.session(session_id)


@router.get("/sessions/{session_id}/events", response_model=RecordPageView)
def events(
    session_id: str,
    historical: Historical,
    offset: Annotated[int, Query(ge=0)] = 0,
    limit: Annotated[int, Query(ge=1, le=500)] = 100,
) -> Any:
    return historical.page(session_id, "historical_events.jsonl", offset, limit)


@router.get("/sessions/{session_id}/series", response_model=RecordPageView)
def series(
    session_id: str,
    historical: Historical,
    offset: Annotated[int, Query(ge=0)] = 0,
    limit: Annotated[int, Query(ge=1, le=500)] = 100,
) -> Any:
    return historical.page(session_id, "historical_samples.jsonl", offset, limit)


@router.get("/sessions/{session_id}/trades", response_model=RecordPageView)
def trades(
    session_id: str,
    historical: Historical,
    offset: Annotated[int, Query(ge=0)] = 0,
    limit: Annotated[int, Query(ge=1, le=500)] = 100,
) -> Any:
    return historical.page(session_id, "historical_events.jsonl", offset, limit, "trade")


@router.get("/sessions/{session_id}/quotes", response_model=RecordPageView)
def quotes(
    session_id: str,
    historical: Historical,
    offset: Annotated[int, Query(ge=0)] = 0,
    limit: Annotated[int, Query(ge=1, le=500)] = 100,
) -> Any:
    return historical.page(session_id, "historical_events.jsonl", offset, limit, "quote")


@router.get("/sessions/{session_id}/quality")
def quality(session_id: str, historical: Historical) -> Any:
    return historical.session(session_id)["quality"]


@router.post("/sessions/{session_id}/verify")
def verify(session_id: str, historical: Historical) -> Any:
    return historical.verify(session_id)


@router.get("/comparisons")
def comparison(run_id: str, session_id: str, historical: Historical) -> Any:
    return historical.comparison(run_id, session_id)
