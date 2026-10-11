"""Shapes registry rows and artifacts into API responses. No number is computed here."""

from __future__ import annotations

import json
from typing import Any

from markettwin_api.artifacts import ArtifactError, RunStore
from markettwin_api.registry import Row
from markettwin_api.service import Research

Json = dict[str, Any]


def _error(row: Row) -> Json | None:
    if not row.get("error_code"):
        return None
    return {
        "code": row["error_code"],
        "message": row.get("error_message") or "",
        "sequence": row.get("error_sequence"),
    }


def run_summary(row: Row) -> Json:
    summary = row.get("summary") or {}
    failed = row["status"] == "failed"
    return {
        "id": row["id"],
        "name": row["name"],
        "scenario_key": row["scenario_key"],
        "seed": row["seed"],
        "horizon_ns": row["horizon_ns"],
        "status": row["status"],
        "replay_status": row["replay_status"],
        "created_at": row["created_at"],
        "started_at": row["started_at"],
        "finished_at": row["finished_at"],
        "processing_ms": row["processing_ms"],
        "engine_version": row["engine_version"],
        "config_digest": row["config_digest"],
        "final_time_ns": summary.get("final_time_ns"),
        "trade_count": summary.get("trade_count"),
        "event_count": summary.get("event_count"),
        "stop_reason": summary.get("stop_reason"),
        "source_kind": row["source_kind"],
        "experiment_id": row["experiment_id"],
        "variant": row["variant"],
        "error": _error(row) if failed else None,
    }


def _parsed(text: str | None) -> Any:
    if not text:
        return None
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return None


def replay_view(row: Row) -> Json:
    result = row.get("replay")
    view: Json = {
        "status": row["replay_status"],
        "checked_at": row["replay_checked_at"],
        "processing_ms": row["replay_ms"],
        "verified": None,
        "code": None,
        "message": None,
        "sequence": None,
        "expected": None,
        "actual": None,
        "expected_raw": None,
        "actual_raw": None,
        "previous_command": None,
        "previous_output": None,
    }
    if result is None or row["replay_status"] in ("not_checked", "running"):
        return view
    view.update(
        verified=result["verified"],
        code=result["code"],
        message=result["message"],
        sequence=result["sequence"] if not result["verified"] else None,
        expected=_parsed(result["expected"]),
        actual=_parsed(result["actual"]),
        expected_raw=result["expected"] or None,
        actual_raw=result["actual"] or None,
        previous_command=_parsed(result["previous_command"]),
        previous_output=_parsed(result["previous_output"]),
    )
    return view


def _agents(config: Json, metrics: Json | None) -> list[Json]:
    scenario = config.get("scenario") or {}
    configured = {agent["id"]: agent for agent in scenario.get("agents", [])}
    accounting = config.get("accounting") or {}
    accounts = {account["agent_id"]: account for account in accounting.get("accounts", [])}
    default_cash = accounting.get("default_initial_cash_minor", 0)
    reports = {agent["agent_id"]: agent for agent in (metrics or {}).get("agents", [])}
    result = []
    for agent_id in config["exchange"]["agents"]:
        agent = configured.get(agent_id)
        account = accounts.get(agent_id)
        result.append(
            {
                "id": agent_id,
                "policy": agent["policy"]["kind"] if agent else None,
                "config": agent,
                "initial_cash_minor": account["initial_cash_minor"] if account else default_cash,
                "credit_limit_minor": account["credit_limit_minor"] if account else None,
                "report": reports.get(agent_id),
            }
        )
    return result


def _shocks(config: Json, recorded: list[Json]) -> list[Json]:
    scenario = config.get("scenario") or {}
    result = []
    for index, definition in enumerate(scenario.get("shocks", [])):
        result.append(
            {
                "index": index,
                "kind": definition["kind"],
                "definition": definition,
                "transitions": [
                    {
                        "phase": item["phase"],
                        "timestamp_ns": item["timestamp_ns"],
                        "sequence": item["sequence"],
                    }
                    for item in recorded
                    if item["index"] == index
                ],
            }
        )
    return result


def run_detail(research: Research, row: Row) -> Json:
    config: Json = row["config"]
    detail = run_summary(row)
    store: RunStore | None = None
    meta: Json | None = None
    metrics: Json | None = None
    manifest: Json | None = None
    index_error: Json | None = None
    files: list[Json] = []
    if row["status"] == "completed":
        store = research.store(row)
        manifest = store.document("manifest.json")
        metrics = store.document("metrics.json")
        files = store.files()
        try:
            meta = store.meta
        except ArtifactError as error:
            index_error = {"code": error.code, "message": error.message, "sequence": None}
    scenario = config.get("scenario") or {}
    sample_count = meta["sample_count"] if meta else 0
    detail.update(
        indexing_ms=row["indexing_ms"],
        config=config,
        summary=row["summary"],
        manifest=manifest,
        metrics=metrics,
        units=metrics["units"] if metrics else None,
        agents=_agents(config, metrics),
        shocks=_shocks(config, meta["shocks"] if meta else []),
        files=files,
        sample_count=sample_count,
        sampling=scenario.get("sampling"),
        availability={
            "artifacts": store is not None,
            "samples": sample_count > 0,
            "books": bool(meta and meta["book_count"] > 0),
            "fundamental": bool(scenario),
            "agents": bool(scenario.get("agents")),
            "trades": bool(meta and meta["trade_count"] > 0),
            "index_error": index_error,
        },
        replay=replay_view(row),
        cell=row["cell"],
    )
    return detail


def experiment_summary(research: Research, row: Row) -> Json:
    progress = research.experiment_progress(row["id"])
    failed = row["status"] == "failed"
    return {
        "id": row["id"],
        "name": row["name"],
        "scenario_key": row["scenario_key"],
        "status": row["status"],
        "created_at": row["created_at"],
        "started_at": row["started_at"],
        "finished_at": row["finished_at"],
        "processing_ms": row["processing_ms"],
        "engine_version": row["engine_version"],
        "seed_count": row["seed_count"],
        "variants": row["variants"],
        "comparison": row["comparison"],
        "total_runs": row["total_runs"],
        "completed_runs": progress["completed"] if progress else row["completed_runs"],
        "failed_runs": progress["failed"] if progress else row["failed_runs"],
        "pairs_completed": row["pairs_completed"],
        "config_digest": row["config_digest"],
        "results_digest": row["results_digest"],
        "headline": row["headline"],
        "error": _error(row) if failed else None,
        "progress": progress,
    }


def experiment_detail(research: Research, row: Row) -> Json:
    detail = experiment_summary(research, row)
    detail.update(config=row["config"], cell_runs=research.cell_runs(row["id"]))
    return detail
