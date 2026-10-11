"""Typed, read-only tool dispatch for the supervised researcher."""

from __future__ import annotations

import json
import time
from collections.abc import Callable
from typing import Any

from pydantic import BaseModel, ConfigDict, Field, ValidationError

from markettwin_api.service import Research, ServiceError
from markettwin_api.validation import validate_experiment_config

Json = dict[str, Any]


class Empty(BaseModel):
    model_config = ConfigDict(extra="forbid")


class ScenarioArg(Empty):
    key: str = Field(pattern=r"^[a-z][a-z0-9_]{0,63}$")


class ExperimentArg(Empty):
    experiment_id: str = Field(pattern=r"^exp-[a-f0-9]{10}$")


class SeedArg(ExperimentArg):
    seed: int = Field(ge=0)


class RunArg(Empty):
    run_id: str = Field(pattern=r"^run-[a-f0-9]{10}$")


class AgentArg(RunArg):
    agent_id: int = Field(ge=1)


class ValidateArg(Empty):
    config: Json


class ToolResult(BaseModel):
    name: str
    data: Json
    duration_ms: int


class ResearchTools:
    """No path or code arguments. Results are capped before entering model context."""

    def __init__(self, research: Research) -> None:
        self.research = research
        self.handlers: dict[str, tuple[type[BaseModel], Callable[[str, Any], Json]]] = {
            "list_scenarios": (Empty, self._list_scenarios),
            "inspect_scenario": (ScenarioArg, self._inspect_scenario),
            "validate_experiment": (ValidateArg, self._validate_experiment),
            "get_experiment_status": (ExperimentArg, self._experiment_status),
            "get_experiment_results": (ExperimentArg, self._experiment_results),
            "get_paired_seed_results": (SeedArg, self._paired_seed),
            "get_run_metrics": (RunArg, self._run_metrics),
            "get_agent_metrics": (AgentArg, self._agent_metrics),
            "inspect_replay_status": (RunArg, self._replay_status),
        }

    def call(self, investigation_id: str, name: str, arguments: Json) -> ToolResult:
        row = self.research.registry.investigation(investigation_id)
        if row is None:
            raise ServiceError(404, "InvestigationNotFound", "no investigation has that ID")
        entry = self.handlers.get(name)
        if entry is None:
            raise ServiceError(422, "UnsupportedTool", "tool is not in the allowlist")
        events = self.research.registry.ai_events(investigation_id)
        if sum(event["operation"] in self.handlers for event in events) >= 64:
            raise ServiceError(429, "ToolLimit", "investigation tool-call limit reached")
        try:
            parsed = entry[0].model_validate(arguments)
        except ValidationError as error:
            raise ServiceError(422, "InvalidToolArguments", "tool arguments are invalid") from error
        started = time.perf_counter()
        try:
            data = entry[1](investigation_id, parsed)
            if len(json.dumps(data)) > 256_000:
                raise ServiceError(413, "ToolResultTooLarge", "tool result exceeds context budget")
            elapsed = round((time.perf_counter() - started) * 1000)
            if elapsed > 10_000:
                raise ServiceError(504, "ToolTimeout", "tool exceeded its time budget")
            self.research.registry.ai_event(
                investigation_id, row["status"], name, "ok", {"duration_ms": elapsed}
            )
            return ToolResult(name=name, data=data, duration_ms=elapsed)
        except ServiceError as error:
            self.research.registry.ai_event(
                investigation_id, row["status"], name, "failed", {"code": error.code}
            )
            raise
        except Exception as error:
            self.research.registry.ai_event(
                investigation_id, row["status"], name, "failed", {"code": "ToolFailed"}
            )
            raise ServiceError(
                502, "ToolFailed", "tool could not retrieve authoritative data"
            ) from error

    def _list_scenarios(self, investigation_id: str, args: Empty) -> Json:
        return {
            "scenarios": [scenario.summary() for scenario in self.research.scenarios.list()][:8]
        }

    def _inspect_scenario(self, investigation_id: str, args: ScenarioArg) -> Json:
        scenario = self.research.scenarios.get(args.key)
        if scenario is None:
            raise ServiceError(404, "ScenarioNotFound", "scenario is not in the catalogue")
        return {"summary": scenario.summary(), "config": scenario.config}

    def _validate_experiment(self, investigation_id: str, args: ValidateArg) -> Json:
        return validate_experiment_config(args.config, self.research.settings)

    def _authorized_experiment(self, investigation_id: str, experiment_id: str) -> Json:
        row = self.research.registry.investigation(investigation_id)
        if row is None or row["experiment_id"] != experiment_id:
            raise ServiceError(403, "ToolNotAuthorized", "experiment is outside this investigation")
        return self.research.require_experiment(experiment_id)

    def _experiment_status(self, investigation_id: str, args: ExperimentArg) -> Json:
        row = self._authorized_experiment(investigation_id, args.experiment_id)
        return {
            "id": row["id"],
            "status": row["status"],
            "completed_runs": row["completed_runs"],
            "failed_runs": row["failed_runs"],
            "progress": self.research.experiment_progress(args.experiment_id),
        }

    def _experiment_results(self, investigation_id: str, args: ExperimentArg) -> Json:
        self._authorized_experiment(investigation_id, args.experiment_id)
        doc = self.research.experiment_document(args.experiment_id)
        return {
            "experiment_id": args.experiment_id,
            "results_digest": doc["results_digest"],
            "config_digest": doc["results"]["config_digest"],
            "comparison": doc["results"]["comparison"],
        }

    def _paired_seed(self, investigation_id: str, args: SeedArg) -> Json:
        self._authorized_experiment(investigation_id, args.experiment_id)
        values = self.research.paired_values(args.experiment_id)
        try:
            index = values["seeds"].index(args.seed)
        except ValueError as error:
            raise ServiceError(404, "SeedNotFound", "seed has no completed pair") from error
        return {
            "seed": args.seed,
            "metrics": {
                metric["name"]: {
                    "unit": metric["unit"],
                    "baseline": metric["baseline"][index],
                    "candidate": metric["candidate"][index],
                    "difference": metric["difference"][index],
                }
                for metric in values["metrics"]
            },
        }

    def _authorized_run(self, investigation_id: str, run_id: str) -> Json:
        run = self.research.require_run(run_id)
        row = self.research.registry.investigation(investigation_id)
        if row is None or not row["experiment_id"] or run["experiment_id"] != row["experiment_id"]:
            raise ServiceError(403, "ToolNotAuthorized", "run is outside this investigation")
        return run

    def _run_metrics(self, investigation_id: str, args: RunArg) -> Json:
        run = self._authorized_run(investigation_id, args.run_id)
        self.research.store(run)
        path = self.research.run_dir(args.run_id) / "metrics.json"
        if path.stat().st_size > 256_000:
            raise ServiceError(413, "ToolResultTooLarge", "metrics artifact exceeds tool budget")
        data: Json = json.loads(path.read_text())
        return data

    def _agent_metrics(self, investigation_id: str, args: AgentArg) -> Json:
        data = self._run_metrics(investigation_id, args)
        agent = next((a for a in data["agents"] if a["agent_id"] == args.agent_id), None)
        if agent is None:
            raise ServiceError(404, "AgentNotFound", "run has no agent with that ID")
        return {"run_id": args.run_id, "agent": agent, "units": data["units"]}

    def _replay_status(self, investigation_id: str, args: RunArg) -> Json:
        run = self._authorized_run(investigation_id, args.run_id)
        return {
            "run_id": args.run_id,
            "status": run["replay_status"],
            "checked_at": run["replay_checked_at"],
            "result": run["replay"],
        }
