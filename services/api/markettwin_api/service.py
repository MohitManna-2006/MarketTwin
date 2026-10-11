"""Runs, experiments, and replay checks as background jobs.

A request validates, records a row, and returns. The native call happens on a worker
thread with the GIL released, so the API stays responsive. Jobs are bounded: a fixed
number run at once and a fixed number may wait.
"""

from __future__ import annotations

import contextlib
import json
import secrets
import shutil
import threading
import time
from collections.abc import Iterator
from concurrent.futures import Future, ThreadPoolExecutor
from pathlib import Path
from typing import Any

import markettwin

from markettwin_api.artifacts import ArtifactError, RunStore
from markettwin_api.registry import Registry, Row, now
from markettwin_api.scenarios import ScenarioCatalogue
from markettwin_api.settings import Settings
from markettwin_api.validation import validate_experiment_config, validate_run_config

Json = dict[str, Any]

# How a native replay code is shown. Anything not listed is a mismatch between the stored
# artifacts and the regenerated run.
_REPLAY_UNSUPPORTED = {"UnsupportedVersion", "UnsupportedSchema"}
_REPLAY_ERRORS = {"IoError", "IncompleteArtifacts", "ResourceLimit", "InvalidJson"}
_CELL_DIGESTS = (
    "config_digest",
    "commands_digest",
    "trace_digest",
    "book_digest",
    "trade_digest",
)


class ServiceError(Exception):
    """A request the service refuses. ``status`` is the HTTP status it maps to."""

    def __init__(self, status: int, code: str, message: str, details: Any = None) -> None:
        super().__init__(message)
        self.status = status
        self.code = code
        self.message = message
        self.details = details


def _new_id(prefix: str) -> str:
    return f"{prefix}-{secrets.token_hex(5)}"


class Research:
    def __init__(self, settings: Settings) -> None:
        self.settings = settings
        for directory in (settings.runs_dir, settings.experiments_dir, settings.derived_dir):
            directory.mkdir(parents=True, exist_ok=True)
        self.registry = Registry(settings.registry_path)
        self.interrupted_at_startup = self.registry.fail_interrupted()
        self.scenarios = ScenarioCatalogue(settings.scenarios_dir)
        self.build = markettwin.build_info()
        self._executor = ThreadPoolExecutor(
            max_workers=settings.max_concurrent_jobs, thread_name_prefix="markettwin-job"
        )
        self._outstanding = 0
        self._lock = threading.Lock()
        self._progress: dict[str, markettwin.ExperimentProgress] = {}

    def close(self) -> None:
        for progress in list(self._progress.values()):
            progress.request_cancel()
        self._executor.shutdown(wait=True, cancel_futures=True)
        self.registry.close()

    # ---- jobs ---------------------------------------------------------------------------

    @property
    def outstanding_jobs(self) -> int:
        return self._outstanding

    def _reserve(self) -> None:
        capacity = self.settings.max_concurrent_jobs + self.settings.max_queued_jobs
        with self._lock:
            if self._outstanding >= capacity:
                raise ServiceError(
                    429,
                    "QueueFull",
                    f"{self._outstanding} jobs are already running or queued; "
                    f"this service holds at most {capacity}",
                )
            self._outstanding += 1

    def _release(self) -> None:
        with self._lock:
            self._outstanding -= 1

    def _start(self, job: Any, *arguments: Any) -> None:
        """Hands a reserved slot to a job. The slot is returned when the job ends, or at
        once if the job cannot be handed over."""
        try:
            future: Future[None] = self._executor.submit(job, *arguments)
        except BaseException:
            self._release()
            raise
        future.add_done_callback(lambda _: self._release())

    @contextlib.contextmanager
    def _reserved(self) -> Iterator[None]:
        """A slot held while a job is being recorded; given back if recording fails."""
        self._reserve()
        try:
            yield
        except BaseException:
            self._release()
            raise

    # ---- paths --------------------------------------------------------------------------

    def run_dir(self, run_id: str) -> Path:
        return self.settings.runs_dir / run_id

    def experiment_dir(self, experiment_id: str) -> Path:
        return self.settings.experiments_dir / experiment_id

    def store(self, run: Row) -> RunStore:
        if run["status"] != "completed":
            raise ServiceError(
                409, "RunNotCompleted", f"run {run['id']} is {run['status']} and has no artifacts"
            )
        return RunStore(
            self.run_dir(run["id"]),
            self.settings.derived_dir / run["id"],
            self.settings.max_samples,
        )

    def require_run(self, run_id: str) -> Row:
        run = self.registry.run(run_id)
        if run is None:
            raise ServiceError(404, "RunNotFound", f"no run has the ID {run_id}")
        return run

    def require_experiment(self, experiment_id: str) -> Row:
        experiment = self.registry.experiment(experiment_id)
        if experiment is None:
            raise ServiceError(
                404, "ExperimentNotFound", f"no experiment has the ID {experiment_id}"
            )
        return experiment

    # ---- runs ---------------------------------------------------------------------------

    def create_run(
        self,
        config: Any,
        scenario_key: str | None = None,
        cell: Json | None = None,
    ) -> Row:
        validation = validate_run_config(config, self.settings)
        if not validation["valid"]:
            raise ServiceError(
                422, "InvalidConfig", "the run config is not valid", validation["errors"]
            )
        if scenario_key is not None and self.scenarios.get(scenario_key) is None:
            raise ServiceError(422, "UnknownScenario", f"no scenario is named {scenario_key}")
        canonical = validation["canonical_config"]
        run_id = _new_id("run")
        with self._reserved():
            self.registry.insert_run(
                {
                    "id": run_id,
                    "name": canonical["run_id"],
                    "scenario_key": scenario_key,
                    "seed": canonical["seed"],
                    "horizon_ns": canonical["horizon_ns"],
                    "status": "pending",
                    "created_at": now(),
                    "engine_version": self.build["engine_version"],
                    "config_digest": validation["config_digest"],
                    "config_json": canonical,
                    "source_kind": "experiment_cell" if cell else "user",
                    "experiment_id": cell["experiment_id"] if cell else None,
                    "variant": cell["variant"] if cell else None,
                    "cell_json": cell,
                }
            )
        self._start(self._execute_run, run_id)
        return self.require_run(run_id)

    def _execute_run(self, run_id: str) -> None:
        run = self.require_run(run_id)
        directory = self.run_dir(run_id)
        self.registry.update_run(run_id, status="running", started_at=now())
        started = time.perf_counter()
        try:
            markettwin.run(json.dumps(run["config"]), directory)
        except markettwin.NativeRunError as error:
            self._fail_run(run_id, started, error.code, str(error), error.sequence)
            shutil.rmtree(directory, ignore_errors=True)
            return
        except Exception as error:
            self._fail_run(run_id, started, "RunFailed", str(error), None)
            shutil.rmtree(directory, ignore_errors=True)
            return
        processing_ms = round((time.perf_counter() - started) * 1000)
        manifest = json.loads((directory / "manifest.json").read_text())
        indexing_started = time.perf_counter()
        index_error: ArtifactError | None = None
        try:
            RunStore(
                directory, self.settings.derived_dir / run_id, self.settings.max_samples
            ).ensure_built()
        except ArtifactError as error:
            index_error = error
        cell = run["cell"]
        if cell is not None:
            cell = {**cell, **_cell_match(cell["expected"], manifest)}
        self.registry.update_run(
            run_id,
            status="completed",
            finished_at=now(),
            processing_ms=processing_ms,
            indexing_ms=round((time.perf_counter() - indexing_started) * 1000),
            summary_json=manifest["summary"],
            cell_json=cell,
            error_code=index_error.code if index_error else None,
            error_message=index_error.message if index_error else None,
        )

    def _fail_run(
        self, run_id: str, started: float, code: str, message: str, sequence: int | None
    ) -> None:
        self.registry.update_run(
            run_id,
            status="failed",
            finished_at=now(),
            processing_ms=round((time.perf_counter() - started) * 1000),
            error_code=code,
            error_message=message,
            error_sequence=sequence,
        )

    # ---- replay -------------------------------------------------------------------------

    def request_replay(self, run_id: str) -> Row:
        run = self.require_run(run_id)
        if run["status"] != "completed":
            raise ServiceError(
                409,
                "RunNotCompleted",
                f"run {run_id} is {run['status']}; only a completed run can be replayed",
            )
        if run["replay_status"] == "running":
            return run
        with self._reserved():
            self.registry.update_run(run_id, replay_status="running")
        self._start(self._execute_replay, run_id)
        return self.require_run(run_id)

    def _execute_replay(self, run_id: str) -> None:
        started = time.perf_counter()
        try:
            result = markettwin.verify_replay(self.run_dir(run_id))
        except Exception as error:
            result = {
                "verified": False,
                "code": getattr(error, "code", "ReplayFailed"),
                "message": str(error),
                "sequence": getattr(error, "sequence", 0),
                "expected": "",
                "actual": "",
                "previous_command": "",
                "previous_output": "",
                "summary": None,
            }
        if result["verified"]:
            status = "verified"
        elif result["code"] in _REPLAY_UNSUPPORTED:
            status = "unsupported_version"
        elif result["code"] in _REPLAY_ERRORS:
            status = "error"
        else:
            status = "mismatch"
        self.registry.update_run(
            run_id,
            replay_status=status,
            replay_checked_at=now(),
            replay_ms=round((time.perf_counter() - started) * 1000),
            replay_json=result,
        )

    # ---- experiments --------------------------------------------------------------------

    def create_experiment(self, config: Any, scenario_key: str | None = None) -> Row:
        validation = validate_experiment_config(config, self.settings)
        if not validation["valid"]:
            raise ServiceError(
                422, "InvalidConfig", "the experiment config is not valid", validation["errors"]
            )
        if scenario_key is not None and self.scenarios.get(scenario_key) is None:
            raise ServiceError(422, "UnknownScenario", f"no scenario is named {scenario_key}")
        canonical = validation["canonical_config"]
        design = validation["design"]
        experiment_id = _new_id("exp")
        with self._reserved():
            self.registry.insert_experiment(
                {
                    "id": experiment_id,
                    "name": canonical["experiment_id"],
                    "scenario_key": scenario_key,
                    "status": "pending",
                    "created_at": now(),
                    "engine_version": self.build["engine_version"],
                    "config_json": canonical,
                    "seed_count": design["seeds"],
                    "variants_json": [variant["name"] for variant in canonical["variants"]],
                    "comparison_json": canonical["comparison"],
                    "total_runs": design["total_runs"],
                }
            )
        self._progress[experiment_id] = markettwin.ExperimentProgress()
        self._start(self._execute_experiment, experiment_id)
        return self.require_experiment(experiment_id)

    def _execute_experiment(self, experiment_id: str) -> None:
        experiment = self.require_experiment(experiment_id)
        progress = self._progress[experiment_id]
        directory = self.experiment_dir(experiment_id)
        directory.mkdir(parents=True, exist_ok=True)
        self.registry.update_experiment(experiment_id, status="running", started_at=now())
        started = time.perf_counter()
        try:
            output = markettwin.run_experiment(
                json.dumps(experiment["config"]), directory / "native", progress
            )
        except Exception as error:
            self.registry.update_experiment(
                experiment_id,
                status="failed",
                finished_at=now(),
                processing_ms=round((time.perf_counter() - started) * 1000),
                error_code=getattr(error, "code", "ExperimentFailed"),
                error_message=str(error),
            )
            self._progress.pop(experiment_id, None)
            return
        (directory / "paired_values.json").write_text(
            json.dumps(output.paired_values, separators=(",", ":"))
        )
        results = output.results
        completed = sum(item["completed"] for item in results["counts"])
        failed = sum(item["failed"] for item in results["counts"])
        if progress.cancel_requested:
            status = "canceled"
        elif failed == 0:
            status = "completed"
        elif completed == 0:
            status = "failed"
        else:
            status = "partially_completed"
        document = json.loads((directory / "native" / "experiment.json").read_text())
        comparison = results["comparison"]
        self.registry.update_experiment(
            experiment_id,
            status=status,
            finished_at=now(),
            processing_ms=round((time.perf_counter() - started) * 1000),
            completed_runs=completed,
            failed_runs=failed,
            pairs_completed=comparison["pairs_completed"] if comparison else None,
            config_digest=results["config_digest"],
            results_digest=document["results_digest"],
            headline_json=_headline(comparison),
        )
        self._progress.pop(experiment_id, None)

    def experiment_progress(self, experiment_id: str) -> Json | None:
        progress = self._progress.get(experiment_id)
        if progress is None:
            return None
        return {
            "total": progress.total,
            "completed": progress.completed,
            "failed": progress.failed,
            "current_run_id": progress.current_run_id or None,
            "cancel_requested": progress.cancel_requested,
        }

    def cancel_experiment(self, experiment_id: str) -> Row:
        experiment = self.require_experiment(experiment_id)
        progress = self._progress.get(experiment_id)
        if progress is None or experiment["status"] not in ("pending", "running"):
            raise ServiceError(
                409,
                "ExperimentNotRunning",
                f"experiment {experiment_id} is {experiment['status']} and cannot be canceled",
            )
        progress.request_cancel()
        return experiment

    def experiment_document(self, experiment_id: str) -> Json:
        experiment = self.require_experiment(experiment_id)
        path = self.experiment_dir(experiment_id) / "native" / "experiment.json"
        if not path.is_file():
            raise ServiceError(
                409,
                "ExperimentNotFinished",
                f"experiment {experiment_id} is {experiment['status']} and has no results document",
            )
        document: Json = json.loads(path.read_text())
        return document

    def paired_values(self, experiment_id: str) -> Json:
        self.experiment_document(experiment_id)
        values: Json = json.loads(
            (self.experiment_dir(experiment_id) / "paired_values.json").read_text()
        )
        return values

    def materialize_cell(self, experiment_id: str, variant: str, seed: int) -> Row:
        """Run one cell of a finished experiment as an ordinary run, so its market can be
        inspected. The core gives a cell the same digests wherever it runs; the result is
        checked against the digests the experiment recorded for it."""
        experiment = self.require_experiment(experiment_id)
        results = self.experiment_document(experiment_id)["results"]
        recorded = next(
            (
                item
                for item in results["runs"]
                if item["variant"] == variant and item["seed"] == seed
            ),
            None,
        )
        if recorded is None:
            raise ServiceError(
                404,
                "CellNotFound",
                f"experiment {experiment_id} has no cell {variant} / seed {seed}",
            )
        if recorded["status"] != "Completed":
            raise ServiceError(
                409,
                "CellFailed",
                "that cell failed in the experiment: "
                f"{recorded['error_code']}: {recorded['error_message']}",
            )
        existing = self.registry.cell_run(experiment_id, variant, seed)
        if existing is not None:
            return existing
        try:
            config = markettwin.experiment_cell_config(
                json.dumps(experiment["config"]), variant, seed
            )
        except markettwin.NativeRunError as error:
            raise ServiceError(422, error.code, str(error)) from error
        expected = {
            key: recorded[key] for key in (*_CELL_DIGESTS, "accounting_digest", "metrics_digest")
        }
        return self.create_run(
            json.loads(config),
            scenario_key=experiment["scenario_key"],
            cell={
                "experiment_id": experiment_id,
                "variant": variant,
                "seed": seed,
                "expected": expected,
                "digests_match": None,
                "mismatched": [],
            },
        )

    def cell_runs(self, experiment_id: str) -> list[Json]:
        return [
            {
                "run_id": row["id"],
                "variant": row["variant"],
                "seed": row["seed"],
                "status": row["status"],
                "digests_match": (row["cell"] or {}).get("digests_match"),
            }
            for row in self.registry.cell_runs(experiment_id)
        ]

    # ---- maintenance --------------------------------------------------------------------

    def delete_run(self, run_id: str) -> None:
        run = self.require_run(run_id)
        if run["status"] in ("pending", "running") or run["replay_status"] == "running":
            raise ServiceError(409, "RunBusy", f"run {run_id} is still being processed")
        with contextlib.suppress(FileNotFoundError):
            shutil.rmtree(self.run_dir(run_id))
        shutil.rmtree(self.settings.derived_dir / run_id, ignore_errors=True)
        self.registry.delete_run(run_id)


def _cell_match(expected: Json, manifest: Json) -> Json:
    summary = manifest["summary"]
    actual = {key: summary[key] for key in _CELL_DIGESTS}
    actual["accounting_digest"] = manifest["artifacts"]["accounting.jsonl"]
    actual["metrics_digest"] = manifest["artifacts"]["metrics.json"]
    mismatched = [key for key, value in expected.items() if actual.get(key) != value]
    return {"digests_match": not mismatched, "mismatched": mismatched}


def _headline(comparison: Json | None) -> Json | None:
    """The first compared metric's paired difference, exactly as the core reported it."""
    if not comparison or not comparison["metrics"]:
        return None
    metric = comparison["metrics"][0]
    return {
        "metric": metric["name"],
        "unit": metric["unit"],
        "scope": metric["scope"],
        "pairs": metric["pairs"],
        "baseline": comparison["baseline"],
        "candidate": comparison["candidate"],
        "difference": metric["difference"],
    }
