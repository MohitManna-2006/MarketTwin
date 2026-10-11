"""Config validation in three layers, reported separately.

1. ``schema``: the structural model, which can name the offending field.
2. ``api``: budgets this service applies (horizon, events, bytes, run count).
3. ``engine``: the core's own validation, which is the authority. Its message is passed
   through verbatim; the core does not say which field it concerns.
"""

from __future__ import annotations

import json
from typing import Any

import markettwin
from pydantic import TypeAdapter, ValidationError

from markettwin_api.config_models import ExperimentConfigModel, RunConfigModel
from markettwin_api.settings import Settings

Json = dict[str, Any]

_RUN_CONFIG = TypeAdapter(RunConfigModel)
_EXPERIMENT_CONFIG = TypeAdapter(ExperimentConfigModel)


def _path(location: tuple[Any, ...], document: Any) -> str:
    """A dotted path into the submitted document. Pydantic inserts the tag of a
    discriminated union into a location; those segments name no key and are dropped."""
    parts: list[str] = []
    current = document
    for position, segment in enumerate(location):
        last = position == len(location) - 1
        in_object = isinstance(current, dict) and segment in current
        in_array = isinstance(current, list) and isinstance(segment, int) and segment < len(current)
        if in_object or in_array:
            parts.append(str(segment))
            current = current[segment]
        elif last and isinstance(current, dict) and isinstance(segment, str):
            parts.append(segment)
    return ".".join(parts)


def _schema_errors(error: ValidationError, document: Any) -> list[Json]:
    result = []
    for item in error.errors(include_url=False, include_input=False):
        result.append(
            {
                "source": "schema",
                "code": item["type"],
                "path": _path(tuple(item["loc"]), document),
                "message": item["msg"],
            }
        )
    return result


def _api_error(path: str, message: str) -> Json:
    return {"source": "api", "code": "ApiLimit", "path": path, "message": message}


def _engine_error(error: markettwin.NativeRunError) -> Json:
    return {"source": "engine", "code": error.code, "path": "", "message": str(error)}


def _run_limits(config: Json, settings: Settings, prefix: str = "") -> list[Json]:
    errors = []
    if config["horizon_ns"] > settings.max_horizon_ns:
        errors.append(
            _api_error(
                f"{prefix}horizon_ns",
                f"this service runs horizons up to {settings.max_horizon_ns:,} ns",
            )
        )
    if config["max_events"] > settings.max_events:
        errors.append(
            _api_error(
                f"{prefix}max_events", f"this service allows up to {settings.max_events:,} events"
            )
        )
    if config["max_trace_bytes"] > settings.max_trace_bytes:
        errors.append(
            _api_error(
                f"{prefix}max_trace_bytes",
                f"this service allows up to {settings.max_trace_bytes:,} bytes per stream",
            )
        )
    return errors


def run_complexity(config: Json) -> Json:
    """Counts that follow from the config alone. No duration is estimated: the number of
    orders, trades, and events depends on what the agents decide."""
    horizon = config["horizon_ns"]
    scenario = config.get("scenario")
    wakeups = 0
    exact = True
    if scenario:
        for agent in scenario["agents"]:
            wakeup = agent["wakeup"]
            if wakeup["first_ns"] > horizon:
                continue
            wakeups += (horizon - wakeup["first_ns"]) // wakeup["interval_ns"] + 1
            exact = exact and wakeup["distribution"] == "Fixed"
    sampling = scenario.get("sampling") if scenario else None
    return {
        "horizon_ns": horizon,
        "agents": len(scenario["agents"]) if scenario else 0,
        "scheduled_wakeups": wakeups if scenario else None,
        "wakeups_exact": exact if scenario else None,
        "fundamental_updates": horizon // scenario["fundamental"]["update_interval_ns"]
        if scenario
        else None,
        "samples": horizon // sampling["interval_ns"] if sampling else None,
        "sampling_depth": sampling["depth"] if sampling else None,
        "shocks": len(scenario["shocks"]) if scenario else 0,
        "scripted_commands": len(config["commands"]),
        "max_events": config["max_events"],
        "max_trace_bytes": config["max_trace_bytes"],
    }


def validate_run_config(config: Any, settings: Settings) -> Json:
    result: Json = {
        "valid": False,
        "errors": [],
        "canonical_config": None,
        "config_digest": None,
        "complexity": None,
    }
    try:
        _RUN_CONFIG.validate_python(config)
    except ValidationError as error:
        result["errors"] = _schema_errors(error, config)
        return result
    result["errors"] = _run_limits(config, settings)
    if result["errors"]:
        return result
    try:
        canonical, digest = markettwin.canonicalize_config(json.dumps(config))
    except markettwin.NativeRunError as error:
        result["errors"] = [_engine_error(error)]
        return result
    result.update(
        valid=True,
        canonical_config=json.loads(canonical),
        config_digest=digest,
        complexity=run_complexity(config),
    )
    return result


def validate_experiment_config(config: Any, settings: Settings) -> Json:
    result: Json = {"valid": False, "errors": [], "canonical_config": None, "design": None}
    try:
        _EXPERIMENT_CONFIG.validate_python(config)
    except ValidationError as error:
        result["errors"] = _schema_errors(error, config)
        return result
    errors = _run_limits(config["base_config"], settings, "base_config.")
    if config["horizon_ns"] is not None and config["horizon_ns"] > settings.max_horizon_ns:
        errors.append(
            _api_error(
                "horizon_ns", f"this service runs horizons up to {settings.max_horizon_ns:,} ns"
            )
        )
    seeds = config["seeds"]
    seed_count = seeds["count"] if isinstance(seeds, dict) else len(seeds)
    total = seed_count * len(config["variants"])
    if total > settings.max_experiment_runs:
        errors.append(
            _api_error(
                "seeds",
                f"{seed_count:,} seeds x {len(config['variants'])} variants is {total:,} runs; "
                f"this service allows {settings.max_experiment_runs:,}",
            )
        )
    if config["limits"]["max_output_bytes"] > settings.max_experiment_output_bytes:
        errors.append(
            _api_error(
                "limits.max_output_bytes",
                f"this service allows up to {settings.max_experiment_output_bytes:,} bytes "
                "of run output",
            )
        )
    result["errors"] = errors
    if errors:
        return result
    try:
        canonical = markettwin.canonicalize_experiment(json.dumps(config))
    except markettwin.NativeRunError as error:
        result["errors"] = [_engine_error(error)]
        return result
    document = json.loads(canonical)
    cell = dict(document["base_config"])
    if document["horizon_ns"] is not None:
        cell["horizon_ns"] = document["horizon_ns"]
    result.update(
        valid=True,
        canonical_config=document,
        design={
            "seeds": len(document["seeds"]),
            "variants": len(document["variants"]),
            "total_runs": len(document["seeds"]) * len(document["variants"]),
            "pairing": "same_seed",
            "comparison": document["comparison"],
            "artifacts": document["artifacts"],
            "per_run": run_complexity(cell),
        },
    )
    return result
