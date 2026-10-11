"""The HTTP application.

Every path the service touches is built from an ID it issued. No request carries a
filesystem path, and the only downloadable files are the six artifacts of a run.
"""

from __future__ import annotations

from collections.abc import AsyncIterator, Awaitable, Callable
from contextlib import asynccontextmanager
from typing import Annotated, Any, Literal

from fastapi import APIRouter, Depends, FastAPI, Query, Request
from fastapi.exceptions import RequestValidationError
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse, JSONResponse, Response

from markettwin_api import API_VERSION, models, views
from markettwin_api.ai_routes import router as ai_router
from markettwin_api.ai_service import AIResearch
from markettwin_api.artifacts import RUN_FILES, ArtifactError, RunStore
from markettwin_api.scenarios import field_catalogue
from markettwin_api.service import Research, ServiceError
from markettwin_api.settings import Settings, load_settings
from markettwin_api.validation import validate_experiment_config, validate_run_config

MAX_PAGE = 1_000
MAX_WINDOW = 2_048
MAX_POINTS = 50_000
RunId = Annotated[str, "run ID"]


def _error(status: int, code: str, message: str, details: Any = None) -> JSONResponse:
    return JSONResponse(
        status_code=status,
        content={"error": {"code": code, "message": message, "details": details}},
    )


def get_research(request: Request) -> Research:
    research: Research = request.app.state.research
    return research


ResearchDep = Annotated[Research, Depends(get_research)]
Offset = Annotated[int, Query(ge=0)]
Limit = Annotated[int, Query(ge=1, le=MAX_PAGE)]

router = APIRouter(prefix=f"/api/v{API_VERSION}")
_ERRORS: dict[int | str, dict[str, Any]] = {
    404: {"model": models.ErrorResponse},
    409: {"model": models.ErrorResponse},
    422: {"model": models.ErrorResponse},
}


# ---- service ------------------------------------------------------------------------------


@router.get("/health", response_model=models.Health)
def health(research: ResearchDep) -> Any:
    build, settings = research.build, research.settings
    return {
        "status": "ok",
        "api_version": API_VERSION,
        "engine_version": build["engine_version"],
        "git_revision": build["git_revision"],
        "compiler": build["compiler"],
        "build_type": build["build_type"],
        "workspace": settings.workspace.name,
        "schemas": {
            "experiment": build["experiment_schema"],
            "accounting": build["accounting_schema"],
            "metrics": build["metrics_schema"],
        },
        "active_jobs": research.outstanding_jobs,
        "max_concurrent_jobs": settings.max_concurrent_jobs,
        "interrupted_at_startup": research.interrupted_at_startup,
        "limits": {
            "max_horizon_ns": settings.max_horizon_ns,
            "max_events": settings.max_events,
            "max_trace_bytes": settings.max_trace_bytes,
            "max_experiment_runs": settings.max_experiment_runs,
            "max_samples": settings.max_samples,
            "max_queued_jobs": settings.max_queued_jobs,
        },
        "counts": {
            "scenarios": len(research.scenarios.list()),
            "runs": research.registry.count_runs(),
            "experiments": research.registry.count_experiments(),
        },
    }


# ---- scenarios ----------------------------------------------------------------------------


@router.get("/scenarios", response_model=list[models.ScenarioSummary])
def list_scenarios(research: ResearchDep) -> Any:
    return [scenario.summary() for scenario in research.scenarios.list()]


@router.get("/scenarios/{key}", response_model=models.ScenarioDetail, responses=_ERRORS)
def get_scenario(key: str, research: ResearchDep) -> Any:
    scenario = research.scenarios.get(key)
    if scenario is None:
        raise ServiceError(404, "ScenarioNotFound", f"no scenario is named {key}")
    return {**scenario.summary(), "config": scenario.config}


@router.get("/schema/fields", response_model=models.FieldCatalogue)
def get_field_catalogue(research: ResearchDep) -> Any:
    return field_catalogue(research.scenarios)


@router.post("/configs/validate", response_model=models.RunValidation)
def validate_config(body: models.ValidateRunRequest, research: ResearchDep) -> Any:
    return validate_run_config(body.config, research.settings)


# ---- runs ---------------------------------------------------------------------------------


@router.post("/runs", response_model=models.RunSummary, status_code=202, responses=_ERRORS)
def create_run(body: models.CreateRunRequest, research: ResearchDep) -> Any:
    return views.run_summary(research.create_run(body.config, body.scenario_key))


@router.get("/runs", response_model=models.RunList)
def list_runs(
    research: ResearchDep,
    offset: Offset = 0,
    limit: Limit = 50,
    include_cells: bool = True,
) -> Any:
    rows = research.registry.runs(limit, offset, include_cells)
    return {
        "total": research.registry.count_runs(include_cells),
        "offset": offset,
        "runs": [views.run_summary(row) for row in rows],
    }


@router.get("/runs/{run_id}", response_model=models.RunDetail, responses=_ERRORS)
def get_run(run_id: str, research: ResearchDep) -> Any:
    return views.run_detail(research, research.require_run(run_id))


@router.delete("/runs/{run_id}", status_code=204, responses=_ERRORS)
def delete_run(run_id: str, research: ResearchDep) -> Response:
    research.delete_run(run_id)
    return Response(status_code=204)


def _store(run_id: str, research: Research) -> RunStore:
    return research.store(research.require_run(run_id))


@router.get("/runs/{run_id}/samples/times", response_model=models.SampleTimes, responses=_ERRORS)
def sample_times(run_id: str, research: ResearchDep) -> Any:
    times = _store(run_id, research).sample_times()
    return JSONResponse({"count": len(times), "timestamp_ns": times})


@router.get("/runs/{run_id}/samples", response_model=models.SampleWindow, responses=_ERRORS)
def sample_window(
    run_id: str,
    research: ResearchDep,
    start: Annotated[int, Query(ge=0)] = 0,
    count: Annotated[int, Query(ge=1, le=MAX_WINDOW)] = 512,
    agents: Annotated[
        str, Query(description="'all', 'none', or comma-separated agent IDs", max_length=4096)
    ] = "all",
) -> Any:
    store = _store(run_id, research)
    if agents == "all":
        agent_ids = list(store.meta["agent_ids"])
        if len(agent_ids) > 64:
            raise ServiceError(
                422, "TooManyAgents", "this run has more than 64 agents; name the ones you need"
            )
    elif agents in ("none", ""):
        agent_ids = []
    else:
        try:
            agent_ids = [int(item) for item in agents.split(",")]
        except ValueError as error:
            raise ServiceError(422, "InvalidAgents", "agents must be integer IDs") from error
    return JSONResponse(store.sample_window(start, count, agent_ids))


@router.get("/runs/{run_id}/series/market", response_model=models.MarketSeries, responses=_ERRORS)
def market_series(
    run_id: str,
    research: ResearchDep,
    max_points: Annotated[int, Query(ge=100, le=MAX_POINTS)] = 20_000,
) -> Any:
    return JSONResponse(_store(run_id, research).market_series(max_points))


@router.get(
    "/runs/{run_id}/series/agents/{agent_id}",
    response_model=models.AgentSeries,
    responses=_ERRORS,
)
def agent_series(
    run_id: str,
    agent_id: int,
    research: ResearchDep,
    max_points: Annotated[int, Query(ge=100, le=MAX_POINTS)] = 20_000,
) -> Any:
    run = research.require_run(run_id)
    accounting = run["config"].get("accounting") or {}
    initial = accounting.get("default_initial_cash_minor", 0)
    for account in accounting.get("accounts", []):
        if account["agent_id"] == agent_id:
            initial = account["initial_cash_minor"]
    return JSONResponse(research.store(run).agent_series(agent_id, max_points, initial))


@router.get("/runs/{run_id}/book", response_model=models.BookAt | None, responses=_ERRORS)
def book_at(run_id: str, research: ResearchDep, time_ns: Annotated[int, Query(ge=0)]) -> Any:
    return _store(run_id, research).book_at(time_ns)


@router.get("/runs/{run_id}/trades", response_model=models.TradePage, responses=_ERRORS)
def trades(
    run_id: str,
    research: ResearchDep,
    from_ns: Annotated[int | None, Query(ge=0)] = None,
    to_ns: Annotated[int | None, Query(ge=0)] = None,
    agent_id: int | None = None,
    offset: Offset = 0,
    limit: Limit = 200,
) -> Any:
    return JSONResponse(_store(run_id, research).trades(from_ns, to_ns, agent_id, offset, limit))


@router.get(
    "/runs/{run_id}/agents/{agent_id}/ledger",
    response_model=models.LedgerPage,
    responses=_ERRORS,
)
def agent_ledger(
    run_id: str, agent_id: int, research: ResearchDep, offset: Offset = 0, limit: Limit = 200
) -> Any:
    return JSONResponse(_store(run_id, research).agent_ledger(agent_id, offset, limit))


@router.get("/runs/{run_id}/events", response_model=models.EventPage, responses=_ERRORS)
def events(
    run_id: str,
    research: ResearchDep,
    stream: Literal["market", "accounting"] = "market",
    channel: Literal["commands", "execution"] | None = None,
    kind: Annotated[list[str] | None, Query(max_length=32)] = None,
    agent_id: int | None = None,
    order_id: int | None = None,
    at_or_before_ns: Annotated[int | None, Query(ge=0)] = None,
    at_or_before_sequence: Annotated[int | None, Query(ge=0)] = None,
    offset: Offset = 0,
    limit: Limit = 200,
) -> Any:
    return JSONResponse(
        _store(run_id, research).events(
            stream,
            channel,
            kind,
            agent_id,
            order_id,
            offset,
            limit,
            at_or_before_ns,
            at_or_before_sequence,
        )
    )


@router.post(
    "/runs/{run_id}/replay", response_model=models.ReplayView, status_code=202, responses=_ERRORS
)
def request_replay(run_id: str, research: ResearchDep) -> Any:
    return views.replay_view(research.request_replay(run_id))


@router.get("/runs/{run_id}/replay", response_model=models.ReplayView, responses=_ERRORS)
def get_replay(run_id: str, research: ResearchDep) -> Any:
    return views.replay_view(research.require_run(run_id))


@router.get("/runs/{run_id}/artifacts/{name}", responses=_ERRORS)
def download_artifact(run_id: str, name: str, research: ResearchDep) -> Response:
    if name not in RUN_FILES:
        raise ServiceError(404, "ArtifactNotFound", f"a run has no artifact named {name}")
    store = _store(run_id, research)
    media = "application/json" if name.endswith(".json") else "application/x-ndjson"
    return FileResponse(store.run_dir / name, media_type=media, filename=f"{run_id}-{name}")


# ---- experiments --------------------------------------------------------------------------


@router.post("/experiments/validate", response_model=models.ExperimentValidation)
def validate_experiment(body: models.ValidateExperimentRequest, research: ResearchDep) -> Any:
    return validate_experiment_config(body.config, research.settings)


@router.post(
    "/experiments", response_model=models.ExperimentSummary, status_code=202, responses=_ERRORS
)
def create_experiment(body: models.CreateExperimentRequest, research: ResearchDep) -> Any:
    row = research.create_experiment(body.config, body.scenario_key)
    return views.experiment_summary(research, row)


@router.get("/experiments", response_model=models.ExperimentList)
def list_experiments(research: ResearchDep, offset: Offset = 0, limit: Limit = 50) -> Any:
    rows = research.registry.experiments(limit, offset)
    return {
        "total": research.registry.count_experiments(),
        "offset": offset,
        "experiments": [views.experiment_summary(research, row) for row in rows],
    }


@router.get(
    "/experiments/{experiment_id}", response_model=models.ExperimentDetail, responses=_ERRORS
)
def get_experiment(experiment_id: str, research: ResearchDep) -> Any:
    return views.experiment_detail(research, research.require_experiment(experiment_id))


@router.get(
    "/experiments/{experiment_id}/results",
    response_model=models.ExperimentResults,
    responses=_ERRORS,
)
def experiment_results(experiment_id: str, research: ResearchDep) -> Any:
    return JSONResponse({"document": research.experiment_document(experiment_id)})


@router.get(
    "/experiments/{experiment_id}/paired-values",
    response_model=models.PairedValues,
    responses=_ERRORS,
)
def experiment_paired_values(experiment_id: str, research: ResearchDep) -> Any:
    return JSONResponse({"document": research.paired_values(experiment_id)})


@router.post(
    "/experiments/{experiment_id}/cancel",
    response_model=models.ExperimentSummary,
    status_code=202,
    responses=_ERRORS,
)
def cancel_experiment(experiment_id: str, research: ResearchDep) -> Any:
    return views.experiment_summary(research, research.cancel_experiment(experiment_id))


@router.post(
    "/experiments/{experiment_id}/cells/materialize",
    response_model=models.RunSummary,
    status_code=202,
    responses=_ERRORS,
)
def materialize_cell(
    experiment_id: str, body: models.MaterializeCellRequest, research: ResearchDep
) -> Any:
    return views.run_summary(research.materialize_cell(experiment_id, body.variant, body.seed))


# ---- application --------------------------------------------------------------------------


def create_app(settings: Settings | None = None) -> FastAPI:
    resolved = settings or load_settings()

    @asynccontextmanager
    async def lifespan(app: FastAPI) -> AsyncIterator[None]:
        app.state.research = Research(resolved)
        app.state.ai = AIResearch(app.state.research)
        try:
            yield
        finally:
            app.state.ai.close()
            app.state.research.close()

    app = FastAPI(
        title="MarketTwin Research API",
        version=API_VERSION,
        description="Runs, experiments, replay verification, and recorded telemetry of the "
        "MarketTwin simulation core. Local, single-user, unauthenticated.",
        lifespan=lifespan,
    )
    app.add_middleware(
        CORSMiddleware,
        allow_origins=list(resolved.cors_origins),
        allow_methods=["GET", "POST", "DELETE"],
        allow_headers=["content-type"],
    )

    @app.middleware("http")
    async def limit_request_size(
        request: Request, call_next: Callable[[Request], Awaitable[Response]]
    ) -> Response:
        length = request.headers.get("content-length")
        if request.method in ("POST", "PUT", "PATCH"):
            if length is None or not length.isdigit():
                return _error(411, "LengthRequired", "a request body needs a Content-Length")
            if int(length) > resolved.max_request_bytes:
                return _error(
                    413,
                    "RequestTooLarge",
                    f"request bodies are limited to {resolved.max_request_bytes:,} bytes",
                )
        return await call_next(request)

    @app.exception_handler(ServiceError)
    async def service_error(_: Request, error: ServiceError) -> JSONResponse:
        return _error(error.status, error.code, error.message, error.details)

    @app.exception_handler(ArtifactError)
    async def artifact_error(_: Request, error: ArtifactError) -> JSONResponse:
        status = 404 if error.code == "NotFound" else 422
        return _error(status, error.code, error.message)

    @app.exception_handler(RequestValidationError)
    async def request_error(_: Request, error: RequestValidationError) -> JSONResponse:
        details = [
            {"path": ".".join(str(part) for part in item["loc"]), "message": item["msg"]}
            for item in error.errors()
        ]
        return _error(422, "InvalidRequest", "the request is not valid", details)

    app.include_router(router)
    app.include_router(ai_router)
    return app
