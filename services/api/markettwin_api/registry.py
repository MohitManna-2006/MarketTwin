"""The registry of runs and experiments: one SQLite file in the workspace.

Artifacts stay on disk in the directories the core wrote. The registry holds what the core
does not record: who asked for a run, when, how long it took in wall-clock time, and what
replay verification last said. Everything else is read back from the artifacts.
"""

from __future__ import annotations

import json
import os
import sqlite3
import threading
from collections.abc import Iterator
from contextlib import contextmanager
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

Row = dict[str, Any]

_SCHEMA = """
CREATE TABLE IF NOT EXISTS runs (
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  scenario_key TEXT,
  seed INTEGER NOT NULL,
  horizon_ns INTEGER NOT NULL,
  status TEXT NOT NULL,
  created_at TEXT NOT NULL,
  started_at TEXT,
  finished_at TEXT,
  processing_ms INTEGER,
  indexing_ms INTEGER,
  engine_version TEXT NOT NULL,
  config_digest TEXT NOT NULL,
  config_json TEXT NOT NULL,
  summary_json TEXT,
  error_code TEXT,
  error_message TEXT,
  error_sequence INTEGER,
  source_kind TEXT NOT NULL DEFAULT 'user',
  experiment_id TEXT,
  variant TEXT,
  cell_json TEXT,
  replay_status TEXT NOT NULL DEFAULT 'not_checked',
  replay_checked_at TEXT,
  replay_ms INTEGER,
  replay_json TEXT
);
CREATE INDEX IF NOT EXISTS runs_created ON runs (created_at DESC);
CREATE INDEX IF NOT EXISTS runs_cell ON runs (experiment_id, variant, seed);

CREATE TABLE IF NOT EXISTS experiments (
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  scenario_key TEXT,
  status TEXT NOT NULL,
  created_at TEXT NOT NULL,
  started_at TEXT,
  finished_at TEXT,
  processing_ms INTEGER,
  engine_version TEXT NOT NULL,
  config_digest TEXT,
  config_json TEXT NOT NULL,
  seed_count INTEGER NOT NULL,
  variants_json TEXT NOT NULL,
  comparison_json TEXT,
  total_runs INTEGER NOT NULL,
  completed_runs INTEGER NOT NULL DEFAULT 0,
  failed_runs INTEGER NOT NULL DEFAULT 0,
  pairs_completed INTEGER,
  results_digest TEXT,
  headline_json TEXT,
  error_code TEXT,
  error_message TEXT
);
CREATE INDEX IF NOT EXISTS experiments_created ON experiments (created_at DESC);
CREATE TABLE IF NOT EXISTS investigations (
  id TEXT PRIMARY KEY,
  question TEXT NOT NULL,
  status TEXT NOT NULL,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  provider TEXT,
  model TEXT,
  plan_json TEXT,
  experiment_id TEXT UNIQUE,
  report_json TEXT,
  error_code TEXT,
  error_message TEXT,
  parent_id TEXT
);
CREATE INDEX IF NOT EXISTS investigations_created ON investigations (created_at DESC);
CREATE TABLE IF NOT EXISTS ai_events (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  investigation_id TEXT NOT NULL REFERENCES investigations(id),
  created_at TEXT NOT NULL,
  stage TEXT NOT NULL,
  operation TEXT NOT NULL,
  status TEXT NOT NULL,
  detail_json TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS ai_events_investigation ON ai_events (investigation_id, id);
CREATE TABLE IF NOT EXISTS ai_messages (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  investigation_id TEXT NOT NULL REFERENCES investigations(id),
  created_at TEXT NOT NULL,
  role TEXT NOT NULL,
  content TEXT NOT NULL,
  evidence_json TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS historical_imports (
  id TEXT PRIMARY KEY,
  request_key TEXT NOT NULL UNIQUE,
  provider TEXT NOT NULL,
  feed TEXT NOT NULL,
  symbol TEXT NOT NULL,
  start_time TEXT,
  end_time TEXT,
  status TEXT NOT NULL,
  created_at TEXT NOT NULL,
  started_at TEXT,
  finished_at TEXT,
  manifest_json TEXT,
  error_code TEXT,
  error_message TEXT
);
CREATE INDEX IF NOT EXISTS historical_imports_created ON historical_imports(created_at DESC);
CREATE TABLE IF NOT EXISTS live_recordings (
  id TEXT PRIMARY KEY,
  provider TEXT NOT NULL,
  feed TEXT NOT NULL,
  status TEXT NOT NULL,
  created_at TEXT NOT NULL,
  finished_at TEXT,
  stop_reason TEXT,
  summary_json TEXT,
  error_code TEXT,
  error_message TEXT
);
CREATE INDEX IF NOT EXISTS live_recordings_created ON live_recordings(created_at DESC);
CREATE TABLE IF NOT EXISTS calibrations (
  id TEXT PRIMARY KEY,
  request_key TEXT NOT NULL UNIQUE,
  spec_key TEXT NOT NULL,
  spec_digest TEXT NOT NULL,
  title TEXT NOT NULL,
  symbol TEXT NOT NULL,
  feed TEXT NOT NULL,
  observation_kind TEXT NOT NULL,
  status TEXT NOT NULL,
  created_at TEXT NOT NULL,
  started_at TEXT,
  finished_at TEXT,
  sessions_json TEXT NOT NULL,
  progress_json TEXT,
  headline_json TEXT,
  error_code TEXT,
  error_message TEXT
);
CREATE INDEX IF NOT EXISTS calibrations_created ON calibrations(created_at DESC);
"""

_JSON_COLUMNS = (
    "config_json",
    "summary_json",
    "cell_json",
    "replay_json",
    "variants_json",
    "comparison_json",
    "headline_json",
    "plan_json",
    "report_json",
    "detail_json",
    "evidence_json",
    "manifest_json",
    "sessions_json",
    "progress_json",
    "fork_json",
    "desk_json",
)

ACTIVE = ("pending", "running")

# Tables whose jobs a process other than the API may own, such as
# scripts/calibrate_aapl.py. Each row records the process that runs it.
_OWNED_TABLES = ("historical_imports", "calibrations")

# Columns added after a table was first created, added to an existing registry on open.
# Phase 6 (ADR 0012): a run's parent and fork stage, its AI desk details, and whether
# its fork report verified; an experiment's desk details.
_ADDED_COLUMNS = (
    ("runs", "parent_run_id", "TEXT"),
    ("runs", "fork_at_ns", "INTEGER"),
    ("runs", "fork_json", "TEXT"),
    ("runs", "fork_status", "TEXT"),
    ("runs", "desk_json", "TEXT"),
    ("experiments", "desk_json", "TEXT"),
)


def _alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def now() -> str:
    """Wall-clock creation and completion times, UTC, to the millisecond."""
    return datetime.now(UTC).isoformat(timespec="milliseconds").replace("+00:00", "Z")


class Registry:
    def __init__(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        self._connection = sqlite3.connect(path, check_same_thread=False, isolation_level=None)
        self._connection.row_factory = sqlite3.Row
        self._lock = threading.RLock()
        with self._cursor() as cursor:
            cursor.execute("PRAGMA journal_mode=WAL")
            cursor.executescript(_SCHEMA)
            added = [(table, "owner_pid", "INTEGER") for table in _OWNED_TABLES]
            for table, column, kind in (*added, *_ADDED_COLUMNS):
                columns = {row[1] for row in cursor.execute(f"PRAGMA table_info({table})")}
                if column not in columns:
                    cursor.execute(f"ALTER TABLE {table} ADD COLUMN {column} {kind}")
            cursor.execute("CREATE INDEX IF NOT EXISTS runs_parent ON runs (parent_run_id)")

    def close(self) -> None:
        with self._lock:
            self._connection.close()

    def historical_import(self, import_id: str) -> Row | None:
        return self._one("SELECT * FROM historical_imports WHERE id = ?", (import_id,))

    def historical_by_key(self, request_key: str) -> Row | None:
        return self._one("SELECT * FROM historical_imports WHERE request_key = ?", (request_key,))

    def historical_imports(self, limit: int, offset: int) -> list[Row]:
        return self._many(
            "SELECT * FROM historical_imports ORDER BY created_at DESC, id DESC LIMIT ? OFFSET ?",
            (limit, offset),
        )

    def insert_historical(self, values: Row) -> bool:
        columns = ", ".join(values)
        marks = ", ".join("?" for _ in values)
        with self._cursor() as cursor:
            result = cursor.execute(
                f"INSERT OR IGNORE INTO historical_imports ({columns}) VALUES ({marks})",
                [_encode(key, value) for key, value in values.items()],
            )
            return result.rowcount == 1

    def update_historical(self, import_id: str, **values: Any) -> None:
        self._update("historical_imports", import_id, values)

    def retire_failed_historical_key(self, request_key: str) -> bool:
        """Keep the failed attempt while allowing the same request to be retried."""
        with self._cursor() as cursor:
            result = cursor.execute(
                "UPDATE historical_imports "
                "SET request_key = request_key || ':failed:' || id "
                "WHERE request_key = ? AND status = 'failed'",
                (request_key,),
            )
            return result.rowcount == 1

    # ---- live recordings ------------------------------------------------------------------

    def calibration(self, calibration_id: str) -> Row | None:
        return self._one("SELECT * FROM calibrations WHERE id = ?", (calibration_id,))

    def calibration_by_key(self, request_key: str) -> Row | None:
        return self._one("SELECT * FROM calibrations WHERE request_key = ?", (request_key,))

    def calibrations(self, limit: int, offset: int) -> list[Row]:
        return self._many(
            "SELECT * FROM calibrations ORDER BY created_at DESC, id DESC LIMIT ? OFFSET ?",
            (limit, offset),
        )

    def insert_calibration(self, values: Row) -> None:
        self._insert("calibrations", values)

    def update_calibration(self, calibration_id: str, **values: Any) -> None:
        self._update("calibrations", calibration_id, values)

    def retire_failed_calibration_key(self, request_key: str) -> None:
        with self._cursor() as cursor:
            cursor.execute(
                "UPDATE calibrations SET request_key = id || ':' || request_key "
                "WHERE request_key = ? AND status = 'failed'",
                (request_key,),
            )

    def insert_live_recording(self, values: Row) -> None:
        self._insert("live_recordings", values)

    def update_live_recording(self, recording_id: str, **values: Any) -> None:
        self._update("live_recordings", recording_id, values)

    def live_recording(self, recording_id: str) -> Row | None:
        return self._one("SELECT * FROM live_recordings WHERE id = ?", (recording_id,))

    def live_recordings(self, limit: int, offset: int = 0) -> list[Row]:
        return self._many(
            "SELECT * FROM live_recordings ORDER BY created_at DESC, id DESC LIMIT ? OFFSET ?",
            (limit, offset),
        )

    def unfinished_live_recordings(self) -> list[Row]:
        """Recordings a previous process left open. A capture is owned by the process that
        started it, so at startup these were interrupted."""
        return self._many(
            "SELECT * FROM live_recordings WHERE status IN ('recording', 'finalizing') "
            "ORDER BY created_at"
        )

    @contextmanager
    def _cursor(self) -> Iterator[sqlite3.Cursor]:
        with self._lock:
            cursor = self._connection.cursor()
            try:
                yield cursor
            finally:
                cursor.close()

    # ---- generic ------------------------------------------------------------------------

    def _insert(self, table: str, values: Row) -> None:
        columns = ", ".join(values)
        marks = ", ".join("?" for _ in values)
        with self._cursor() as cursor:
            cursor.execute(
                f"INSERT INTO {table} ({columns}) VALUES ({marks})",
                [_encode(key, value) for key, value in values.items()],
            )

    def _update(self, table: str, row_id: str, values: Row) -> None:
        assignments = ", ".join(f"{key} = ?" for key in values)
        with self._cursor() as cursor:
            cursor.execute(
                f"UPDATE {table} SET {assignments} WHERE id = ?",
                [*(_encode(key, value) for key, value in values.items()), row_id],
            )

    def _one(self, query: str, parameters: tuple[Any, ...]) -> Row | None:
        with self._cursor() as cursor:
            row = cursor.execute(query, parameters).fetchone()
        return _decode(row) if row is not None else None

    def _many(self, query: str, parameters: tuple[Any, ...] = ()) -> list[Row]:
        with self._cursor() as cursor:
            rows = cursor.execute(query, parameters).fetchall()
        return [_decode(row) for row in rows]

    def _count(self, query: str, parameters: tuple[Any, ...] = ()) -> int:
        with self._cursor() as cursor:
            return int(cursor.execute(query, parameters).fetchone()[0])

    # ---- runs ---------------------------------------------------------------------------

    def insert_run(self, values: Row) -> None:
        self._insert("runs", values)

    def update_run(self, run_id: str, **values: Any) -> None:
        self._update("runs", run_id, values)

    def run(self, run_id: str) -> Row | None:
        return self._one("SELECT * FROM runs WHERE id = ?", (run_id,))

    def runs(self, limit: int, offset: int, include_cells: bool = True) -> list[Row]:
        where = "" if include_cells else "WHERE source_kind IN ('user', 'fork')"
        return self._many(
            f"SELECT * FROM runs {where} ORDER BY created_at DESC, id DESC LIMIT ? OFFSET ?",
            (limit, offset),
        )

    def count_runs(self, include_cells: bool = True) -> int:
        where = "" if include_cells else "WHERE source_kind IN ('user', 'fork')"
        return self._count(f"SELECT COUNT(*) FROM runs {where}")

    def child_runs(self, parent_ids: list[str]) -> list[Row]:
        if not parent_ids:
            return []
        marks = ", ".join("?" for _ in parent_ids)
        return self._many(
            f"SELECT * FROM runs WHERE parent_run_id IN ({marks}) ORDER BY created_at, id",
            tuple(parent_ids),
        )

    def cell_run(self, experiment_id: str, variant: str, seed: int) -> Row | None:
        return self._one(
            "SELECT * FROM runs WHERE experiment_id = ? AND variant = ? AND seed = ? "
            "AND status != 'failed' ORDER BY created_at DESC LIMIT 1",
            (experiment_id, variant, seed),
        )

    def cell_runs(self, experiment_id: str) -> list[Row]:
        return self._many(
            "SELECT * FROM runs WHERE experiment_id = ? ORDER BY created_at", (experiment_id,)
        )

    def delete_run(self, run_id: str) -> None:
        with self._cursor() as cursor:
            cursor.execute("DELETE FROM runs WHERE id = ?", (run_id,))

    # ---- experiments --------------------------------------------------------------------

    def insert_experiment(self, values: Row) -> None:
        self._insert("experiments", values)

    def update_experiment(self, experiment_id: str, **values: Any) -> None:
        self._update("experiments", experiment_id, values)

    def experiment(self, experiment_id: str) -> Row | None:
        return self._one("SELECT * FROM experiments WHERE id = ?", (experiment_id,))

    def experiments(self, limit: int, offset: int) -> list[Row]:
        return self._many(
            "SELECT * FROM experiments ORDER BY created_at DESC, id DESC LIMIT ? OFFSET ?",
            (limit, offset),
        )

    def count_experiments(self) -> int:
        return self._count("SELECT COUNT(*) FROM experiments")

    # ---- AI investigations --------------------------------------------------------------

    def insert_investigation(self, values: Row) -> None:
        self._insert("investigations", values)

    def insert_investigation_bounded(
        self, values: Row, total_limit: int, active_limit: int
    ) -> bool:
        with self._cursor() as cursor:
            total = cursor.execute("SELECT COUNT(*) FROM investigations").fetchone()[0]
            active = cursor.execute(
                "SELECT COUNT(*) FROM investigations WHERE status IN "
                "('CREATED', 'PLANNING', 'AWAITING_APPROVAL', 'APPROVED', 'EXECUTING', 'ANALYZING')"
            ).fetchone()[0]
            if total >= total_limit or active >= active_limit:
                return False
            columns = ", ".join(values)
            marks = ", ".join("?" for _ in values)
            cursor.execute(
                f"INSERT INTO investigations ({columns}) VALUES ({marks})",
                [_encode(key, value) for key, value in values.items()],
            )
            return True

    def investigation(self, investigation_id: str) -> Row | None:
        return self._one("SELECT * FROM investigations WHERE id = ?", (investigation_id,))

    def investigations(self, limit: int, offset: int) -> list[Row]:
        return self._many(
            "SELECT * FROM investigations ORDER BY created_at DESC, id DESC LIMIT ? OFFSET ?",
            (limit, offset),
        )

    def count_investigations(self) -> int:
        return self._count("SELECT COUNT(*) FROM investigations")

    def active_investigations(self) -> int:
        return self._count(
            "SELECT COUNT(*) FROM investigations WHERE status IN "
            "('CREATED', 'PLANNING', 'AWAITING_APPROVAL', 'APPROVED', 'EXECUTING', 'ANALYZING')"
        )

    def transition_investigation(
        self, investigation_id: str, expected: tuple[str, ...], status: str, **values: Any
    ) -> bool:
        """A compare-and-swap transition; repeat approvals cannot submit a second job."""
        assignments = ", ".join(f"{key} = ?" for key in values)
        extra = f", {assignments}" if assignments else ""
        marks = ", ".join("?" for _ in expected)
        with self._cursor() as cursor:
            cursor.execute(
                f"UPDATE investigations SET status = ?, updated_at = ?{extra} "
                f"WHERE id = ? AND status IN ({marks})",
                [
                    status,
                    now(),
                    *(_encode(k, v) for k, v in values.items()),
                    investigation_id,
                    *expected,
                ],
            )
            return cursor.rowcount == 1

    def ai_event(
        self,
        investigation_id: str,
        stage: str,
        operation: str,
        status: str,
        detail: Row | None = None,
    ) -> None:
        self._insert(
            "ai_events",
            {
                "investigation_id": investigation_id,
                "created_at": now(),
                "stage": stage,
                "operation": operation,
                "status": status,
                "detail_json": detail or {},
            },
        )

    def ai_events(self, investigation_id: str) -> list[Row]:
        return self._many(
            "SELECT * FROM ai_events WHERE investigation_id = ? ORDER BY id", (investigation_id,)
        )

    def ai_message(
        self, investigation_id: str, role: str, content: str, evidence: list[str] | None = None
    ) -> None:
        self._insert(
            "ai_messages",
            {
                "investigation_id": investigation_id,
                "created_at": now(),
                "role": role,
                "content": content,
                "evidence_json": evidence or [],
            },
        )

    def ai_messages(self, investigation_id: str) -> list[Row]:
        return self._many(
            "SELECT * FROM ai_messages WHERE investigation_id = ? ORDER BY id", (investigation_id,)
        )

    # ---- jobs ---------------------------------------------------------------------------

    def active_jobs(self) -> int:
        return (
            self._count("SELECT COUNT(*) FROM runs WHERE status IN ('pending', 'running')")
            + self._count("SELECT COUNT(*) FROM experiments WHERE status IN ('pending', 'running')")
            + self._count("SELECT COUNT(*) FROM runs WHERE replay_status = 'running'")
        )

    def fail_interrupted(self) -> int:
        """A job is owned by the process that started it. After a restart nothing is running,
        so whatever still says so was interrupted and is recorded as such."""
        stamp = now()
        with self._cursor() as cursor:
            runs = cursor.execute(
                "UPDATE runs SET status = 'failed', finished_at = ?, "
                "error_code = 'WorkerInterrupted', "
                "error_message = 'the API process stopped before this run finished' "
                "WHERE status IN ('pending', 'running')",
                (stamp,),
            ).rowcount
            experiments = cursor.execute(
                "UPDATE experiments SET status = 'failed', finished_at = ?, "
                "error_code = 'WorkerInterrupted', "
                "error_message = 'the API process stopped before this experiment finished' "
                "WHERE status IN ('pending', 'running')",
                (stamp,),
            ).rowcount
            cursor.execute(
                "UPDATE runs SET replay_status = 'not_checked' WHERE replay_status = 'running'"
            )
            cursor.execute(
                "UPDATE investigations SET status = 'FAILED', updated_at = ?, "
                "error_code = 'WorkerInterrupted', "
                "error_message = 'the API process stopped during this investigation' "
                "WHERE status IN ('PLANNING', 'APPROVED', 'ANALYZING')",
                (stamp,),
            )
            # A job another live process owns is still running; leave it alone.
            owned = 0
            for table, message in (
                ("historical_imports", "the process stopped during historical import"),
                ("calibrations", "the process stopped during this calibration"),
            ):
                for row_id, owner in cursor.execute(
                    f"SELECT id, owner_pid FROM {table} WHERE status IN ('pending', 'running')"
                ).fetchall():
                    if owner is not None and owner != os.getpid() and _alive(int(owner)):
                        continue
                    cursor.execute(
                        f"UPDATE {table} SET status = 'failed', finished_at = ?, "
                        "error_code = 'WorkerInterrupted', error_message = ? WHERE id = ?",
                        (stamp, message, row_id),
                    )
                    owned += 1
        return int(runs) + int(experiments) + owned


def _encode(key: str, value: Any) -> Any:
    if key in _JSON_COLUMNS and value is not None and not isinstance(value, str):
        return json.dumps(value, separators=(",", ":"))
    return value


def _decode(row: sqlite3.Row) -> Row:
    result: Row = {}
    for key in row.keys():  # noqa: SIM118 - sqlite3.Row is not a mapping
        value = row[key]
        if key in _JSON_COLUMNS and value is not None:
            result[key.removesuffix("_json")] = json.loads(value)
        elif key in _JSON_COLUMNS:
            result[key.removesuffix("_json")] = None
        else:
            result[key] = value
    return result
