"""Durable calibration jobs over verified historical sessions (ADR 0011).

A calibration names a committed spec and the historical sessions to fit, test, and
validate against. The request is checked in full before anything runs: the spec, the base
config, every candidate config, and every session's provider, symbol, and window. The job
then runs on a worker thread and publishes one immutable artifact directory.
"""

from __future__ import annotations

import hashlib
import json
import random
import re
import secrets
import threading
from datetime import UTC, date, datetime, timedelta
from decimal import Decimal
from pathlib import Path
from typing import Any

import markettwin
from pydantic import ValidationError

from markettwin_api.calibration_engine import (
    CalibrationError,
    Context,
    SessionInput,
    run_calibration,
    verify,
)
from markettwin_api.calibration_spec import (
    CalibrationSpec,
    base_problems,
    candidate_config,
    canonical_json,
    deviations,
    draw_candidates,
    local_day,
    parse_utc,
    problems,
    session_window,
    spec_digest,
    utc_ns,
    utc_text,
    window_ns,
)
from markettwin_api.historical_providers import Acquisition
from markettwin_api.historical_service import HistoricalResearch
from markettwin_api.registry import Row, now
from markettwin_api.service import Research, ServiceError

Json = dict[str, Any]
_CALIBRATION_ID = re.compile(r"cal-[a-f0-9]{10}\Z")
_SPEC_KEY = re.compile(r"[a-z0-9_]{1,64}\Z")
# Ten fabricated trading days for the offline pipeline (all in US daylight time).
FIXTURE_DAYS = tuple(
    date(2026, 10, 5) + timedelta(days=offset) for offset in (0, 1, 2, 3, 4, 7, 8, 9, 10, 11)
)


def observation_kind(provider: str, feed: str) -> str:
    if provider == "alpaca" and feed == "iex":
        return "OBSERVED IEX DATA"
    return "FABRICATED TEST DATA"


class CalibrationResearch:
    def __init__(self, research: Research, historical: HistoricalResearch) -> None:
        self.research = research
        self.historical = historical
        self.registry = research.registry
        self.root = research.settings.calibrations_dir
        self.root.mkdir(parents=True, exist_ok=True)
        self._stop = threading.Event()

    def close(self) -> None:
        self._stop.set()

    # ---- specs --------------------------------------------------------------------------

    def _load_spec(self, key: str) -> tuple[CalibrationSpec, Json]:
        if _SPEC_KEY.fullmatch(key) is None:
            raise ServiceError(404, "UnknownCalibrationSpec", "no calibration spec has that name")
        path = self.research.settings.calibration_specs_dir / f"{key}.json"
        if not path.is_file():
            raise ServiceError(404, "UnknownCalibrationSpec", "no calibration spec has that name")
        try:
            spec = CalibrationSpec.model_validate(json.loads(path.read_text()))
        except (ValueError, ValidationError) as error:
            raise ServiceError(
                500, "InvalidCalibrationSpec", f"calibration spec {key} is invalid"
            ) from error
        base_path = self.research.settings.scenarios_dir / spec.base_config
        if not base_path.is_file():
            raise ServiceError(500, "InvalidCalibrationSpec", "the spec's base config is missing")
        base = json.loads(base_path.read_text())
        return spec, base

    def specs(self) -> Json:
        items = []
        directory = self.research.settings.calibration_specs_dir
        for path in sorted(directory.glob("*.json")) if directory.is_dir() else []:
            spec, base = self._load_spec(path.stem)
            items.append(
                {
                    "key": path.stem,
                    "title": spec.title,
                    "symbol": spec.symbol,
                    "feed": spec.feed,
                    "window": spec.window.model_dump(),
                    "split": spec.split.model_dump(),
                    "search": spec.search.model_dump(),
                    "grid": spec.grid.model_dump(),
                    "grid_size": spec.grid_size,
                    "sessions_required": spec.session_count,
                    "spec_sha256": spec_digest(spec),
                    "deviations_from_adr": deviations(spec),
                    "problems": problems(spec) + base_problems(spec, base),
                }
            )
        return {"specs": items}

    # ---- create ---------------------------------------------------------------------------

    def _sessions(self, spec: CalibrationSpec, session_ids: list[str]) -> list[SessionInput]:
        if len(set(session_ids)) != len(session_ids):
            raise ServiceError(422, "InvalidCalibrationSessions", "sessions must be distinct")
        if len(session_ids) != spec.session_count:
            raise ServiceError(
                422,
                "InvalidCalibrationSessions",
                f"the spec needs exactly {spec.session_count} sessions",
            )
        expected_provider = "alpaca" if spec.feed == "iex" else "local_file"
        rows: list[tuple[date, Row]] = []
        for session_id in session_ids:
            try:
                row = self.historical.require(session_id)
            except ServiceError as error:
                raise ServiceError(
                    422, "InvalidCalibrationSessions", f"{session_id} is not a historical session"
                ) from error
            if row["status"] != "completed":
                raise ServiceError(
                    422, "InvalidCalibrationSessions", f"{session_id} has not completed"
                )
            if (row["provider"], row["feed"], row["symbol"]) != (
                expected_provider,
                spec.feed,
                spec.symbol,
            ):
                raise ServiceError(
                    422,
                    "InvalidCalibrationSessions",
                    f"{session_id} is not a {spec.symbol} {spec.feed} session from "
                    f"{expected_provider}",
                )
            if not row["start_time"] or not row["end_time"]:
                raise ServiceError(
                    422, "InvalidCalibrationSessions", f"{session_id} has no requested window"
                )
            start, end = parse_utc(row["start_time"]), parse_utc(row["end_time"])
            day = local_day(spec, start)
            if (start, end) != session_window(spec, day):
                raise ServiceError(
                    422,
                    "InvalidCalibrationSessions",
                    f"{session_id} does not cover {spec.window.start_local} to "
                    f"{spec.window.end_local} {spec.window.timezone}",
                )
            rows.append((day, row))
        rows.sort(key=lambda item: item[0])
        days = [day for day, _ in rows]
        if len(set(days)) != len(days):
            raise ServiceError(
                422, "InvalidCalibrationSessions", "two sessions fall on the same day"
            )
        roles = (
            ["train"] * spec.split.train
            + ["validation"] * spec.split.validation
            + ["test"] * spec.split.test
        )
        sessions = []
        for (day, row), role in zip(rows, roles, strict=True):
            directory = self.historical.session_dir(row["id"])
            quality = json.loads((directory / "quality_report.json").read_text())
            sessions.append(
                SessionInput(
                    id=row["id"],
                    day=day.isoformat(),
                    role=role,
                    directory=directory,
                    start=row["start_time"],
                    end=row["end_time"],
                    start_ns=utc_ns(parse_utc(row["start_time"])),
                    end_ns=utc_ns(parse_utc(row["end_time"])),
                    canonical_sha256=(row.get("manifest") or {}).get("canonical_sha256"),
                    gaps_over_five_minutes=int(quality.get("gaps_over_five_minutes", 0)),
                )
            )
        return sessions

    def create(self, spec_key: str, session_ids: list[str]) -> Row:
        spec, base = self._load_spec(spec_key)
        found = problems(spec) + base_problems(spec, base)
        if spec.feed == "iex":
            found += [f"observed data requires the ADR: {item}" for item in deviations(spec)]
        if found:
            raise ServiceError(422, "InvalidCalibrationSpec", "; ".join(found))
        sessions = self._sessions(spec, session_ids)
        # Every candidate config must be valid before any session is read.
        for parameters in [None, *draw_candidates(spec)]:
            try:
                markettwin.canonicalize_config(
                    candidate_config(
                        base, parameters, 15_000, spec.search.stage1_seed_base, "check"
                    )
                )
            except markettwin.NativeRunError as error:
                raise ServiceError(
                    422, "InvalidCalibrationSpec", f"a candidate config is invalid: {error}"
                ) from error
        digest = spec_digest(spec)
        request_key = hashlib.sha256(
            canonical_json([digest, sorted(session.id for session in sessions)])
        ).hexdigest()
        existing = self.registry.calibration_by_key(request_key)
        if existing:
            if existing["status"] != "failed":
                return existing
            self.registry.retire_failed_calibration_key(request_key)
        calibration_id = "cal-" + secrets.token_hex(5)
        first = self.historical.require(sessions[0].id)
        with self.research._reserved():
            self.registry.insert_calibration(
                {
                    "id": calibration_id,
                    "request_key": request_key,
                    "spec_key": spec_key,
                    "spec_digest": digest,
                    "title": spec.title,
                    "symbol": spec.symbol,
                    "feed": spec.feed,
                    "observation_kind": observation_kind(first["provider"], first["feed"]),
                    "status": "pending",
                    "created_at": now(),
                    "sessions_json": [
                        {"session_id": session.id, "day": session.day, "role": session.role}
                        for session in sessions
                    ],
                    "progress_json": {"phase": "queued", "completed": 0, "total": 0},
                }
            )
        self.research._start(self._execute, calibration_id, spec_key)
        return self.require(calibration_id)

    def _execute(self, calibration_id: str, spec_key: str) -> None:
        row = self.require(calibration_id)
        self.registry.update_calibration(calibration_id, status="running", started_at=now())

        def progress(phase: str, completed: int, total: int) -> None:
            self.registry.update_calibration(
                calibration_id,
                progress_json={"phase": phase, "completed": completed, "total": total},
            )

        try:
            spec, base = self._load_spec(spec_key)
            sessions = self._sessions(spec, [item["session_id"] for item in row["sessions"]])
            context = Context(
                calibration_id=calibration_id,
                spec=spec,
                base=base,
                observation_kind=row["observation_kind"],
                workers=self.research.settings.calibration_workers,
                progress=progress,
                cancelled=self._stop.is_set,
                build=self.research.build,
            )
            report = run_calibration(context, sessions, self.directory(calibration_id))
            self.registry.update_calibration(
                calibration_id,
                status="completed",
                finished_at=now(),
                progress_json={"phase": "published", "completed": 1, "total": 1},
                headline_json={
                    "outcome": report["outcome"],
                    "label": report["label"],
                    "selected": report["selected"],
                    "test_score_ppm": (report["test"]["selected"] or {}).get("score_ppm"),
                    "baseline_test_score_ppm": (report["test"]["baseline"] or {}).get("score_ppm"),
                    "facts_within": report["acceptance"]["facts_within"],
                },
            )
        except (CalibrationError, ServiceError, markettwin.NativeRunError) as error:
            self.registry.update_calibration(
                calibration_id,
                status="failed",
                finished_at=now(),
                error_code=getattr(error, "code", "CalibrationFailed"),
                error_message=str(error),
            )
        except Exception:
            self.registry.update_calibration(
                calibration_id,
                status="failed",
                finished_at=now(),
                error_code="CalibrationFailed",
                error_message="the calibration failed unexpectedly",
            )

    # ---- reads ----------------------------------------------------------------------------

    def directory(self, calibration_id: str) -> Path:
        return self.root / calibration_id

    def require(self, calibration_id: str) -> Row:
        if _CALIBRATION_ID.fullmatch(calibration_id) is None:
            raise ServiceError(404, "CalibrationNotFound", "calibration ID is unknown")
        row = self.registry.calibration(calibration_id)
        if row is None:
            raise ServiceError(404, "CalibrationNotFound", "calibration ID is unknown")
        return row

    def list(self, limit: int, offset: int) -> Json:
        rows = self.registry.calibrations(limit, offset)
        return {"calibrations": rows, "offset": offset, "count": len(rows)}

    def _artifact(self, calibration_id: str, name: str) -> Any:
        row = self.require(calibration_id)
        if row["status"] != "completed":
            raise ServiceError(409, "CalibrationNotCompleted", "the calibration has not finished")
        return json.loads((self.directory(calibration_id) / name).read_text())

    def report(self, calibration_id: str) -> Json:
        report: Json = self._artifact(calibration_id, "report.json")
        observed = self._artifact(calibration_id, "observed_facts.json")
        spec = self._artifact(calibration_id, "spec.json")
        return {
            "calibration": self.require(calibration_id),
            "report": report,
            "observed": observed["sessions"],
            "spec": spec["spec"],
            "units": observed["sessions"][0]["facts"]["units"],
            "groups": observed["sessions"][0]["facts"]["groups"],
        }

    def selected_config(self, calibration_id: str) -> Json:
        config = self._artifact(calibration_id, "selected_config.json")
        if config is None:
            raise ServiceError(404, "NoSelectedConfig", "no candidate was selected")
        result: Json = config
        return result

    def candidates(self, calibration_id: str, stage: str, offset: int, limit: int) -> Json:
        self._artifact(calibration_id, "report.json")
        if stage not in ("stage1", "stage2", "test"):
            raise ServiceError(422, "UnknownStage", "stage is stage1, stage2, or test")
        items: list[Json] = []
        matched = 0
        with (self.directory(calibration_id) / "candidates.jsonl").open() as stream:
            for raw in stream:
                line = json.loads(raw)
                if line["stage"] != stage:
                    continue
                if matched >= offset and len(items) < limit:
                    items.append(line)
                matched += 1
        return {"stage": stage, "offset": offset, "candidates": items, "total": matched}

    def verify(self, calibration_id: str) -> Json:
        self._artifact(calibration_id, "report.json")
        row = self.require(calibration_id)
        sessions = {
            item["session_id"]: self.historical.session_dir(item["session_id"])
            for item in row["sessions"]
        }
        return verify(self.directory(calibration_id), sessions)

    # ---- fabricated sessions -------------------------------------------------------------

    def fixture_sessions(self) -> Json:
        """Ten fabricated AAPL sessions in the spec window, for the offline pipeline.

        They go through the ordinary Phase 4A import as provider ``local_file`` and are
        labelled FABRICATED TEST DATA everywhere they appear.
        """
        spec, _ = self._load_spec("fixture_smoke")
        ids = []
        for index, day in enumerate(FIXTURE_DAYS):
            start, end = session_window(spec, day)

            def prepare(raw_dir: Path, index: int = index, start_ns: int = utc_ns(start)) -> Any:
                raw_dir.mkdir(parents=True, exist_ok=False)
                path = raw_dir / "records.jsonl"
                path.write_text(fabricated_records(index, start_ns, window_ns(spec)))
                return Acquisition(
                    path,
                    {
                        "symbol": "AAPL",
                        "provider": "local_file",
                        "feed": "fabricated_l1",
                        "scope": "single_venue",
                        "license": "test_fixture_only",
                        "acquired_at": now(),
                    },
                )

            row = self.historical.import_prepared(
                "local_file",
                "fabricated_l1",
                "AAPL",
                utc_text(start),
                utc_text(end),
                f"calibration-fixture-{day.isoformat()}",
                prepare,
            )
            if row["status"] != "completed":
                raise ServiceError(
                    500, "FixtureImportFailed", row.get("error_message") or "import failed"
                )
            ids.append(row["id"])
        return {"session_ids": ids, "observation_kind": "FABRICATED TEST DATA"}


def fabricated_records(index: int, start_ns: int, length_ns: int) -> str:
    """A deterministic fabricated L1 market: a cent-grid random walk around $250 with a
    one- to three-cent spread, a quote every ~2 s and a trade every ~5 s. ``index`` varies
    the walk and the activity from day to day."""
    rng = random.Random(f"markettwin-calibration-fixture-{index}")
    lines: list[str] = []
    mid_cents = 25_000 + rng.randint(-200, 200)
    quote_gap = 1_500_000_000 + index * 100_000_000
    trade_gap = 4_000_000_000 + (index % 3) * 1_000_000_000
    next_quote = 0
    next_trade = 700_000_000
    bid = ask = 0
    while True:
        if next_quote <= next_trade:
            if next_quote >= length_ns:
                break
            mid_cents += rng.choice((-1, 0, 0, 1))
            half = rng.choice((0, 1, 1, 2))
            bid, ask = mid_cents - half - (1 if half == 0 else 0), mid_cents + half
            if bid >= ask:
                ask = bid + 1
            lines.append(
                json.dumps(
                    {
                        "kind": "quote",
                        "symbol": "AAPL",
                        "t": _timestamp(start_ns + next_quote),
                        "bp": str(Decimal(bid) / 100),
                        "ap": str(Decimal(ask) / 100),
                        "bs": rng.choice((100, 200, 300, 400)),
                        "as": rng.choice((100, 200, 300, 400)),
                        "x": "V",
                    },
                    separators=(",", ":"),
                )
            )
            next_quote += rng.randint(quote_gap // 2, quote_gap * 3 // 2)
        else:
            if next_trade >= length_ns:
                break
            price = rng.choice((bid, ask)) if bid else mid_cents
            lines.append(
                json.dumps(
                    {
                        "kind": "trade",
                        "symbol": "AAPL",
                        "t": _timestamp(start_ns + next_trade),
                        "p": str(Decimal(price) / 100),
                        "s": rng.choice((1, 5, 10, 25, 50, 100)),
                        "x": "V",
                    },
                    separators=(",", ":"),
                )
            )
            next_trade += int(rng.expovariate(1.0) * trade_gap) + 1_000_000
    return "\n".join(lines) + "\n"


def _timestamp(ns: int) -> str:
    seconds, fraction = divmod(ns, 10**9)
    clock = datetime.fromtimestamp(seconds, UTC).strftime("%Y-%m-%dT%H:%M:%S")
    return f"{clock}.{fraction:09d}Z"
