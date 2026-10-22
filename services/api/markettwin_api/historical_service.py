"""Durable historical import jobs and bounded reads of native observed-market artifacts."""

from __future__ import annotations

import hashlib
import json
import os
import re
import secrets
from collections.abc import Callable
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

import markettwin

from markettwin_api.historical_providers import Acquisition, AlpacaProvider, LocalFileProvider
from markettwin_api.registry import Row, now
from markettwin_api.service import Research, ServiceError
from markettwin_api.views import run_detail

Json = dict[str, Any]
_HISTORICAL_ID = re.compile(r"hist-[a-f0-9]{10}\Z")


def _utc_instant(value: str) -> datetime:
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as error:
        raise ServiceError(
            422, "InvalidProviderRequest", "historical ranges must be RFC3339 timestamps"
        ) from error
    if parsed.tzinfo is None or parsed.utcoffset() != UTC.utcoffset(parsed):
        raise ServiceError(
            422, "InvalidProviderRequest", "historical ranges must use an explicit UTC offset"
        )
    return parsed


def observation_kind(provider: str, feed: str) -> str:
    """The label every screen shows for a session, so test data is never taken for real."""
    if provider in ("local_file", "mock_live"):
        return "FABRICATED TEST DATA"
    if provider == "alpaca_live":
        return "ALPACA TEST STREAM DATA" if feed == "test" else "RECORDED LIVE DATA"
    return "REAL HISTORICAL DATA"


class HistoricalResearch:
    def __init__(self, research: Research) -> None:
        self.research = research
        self.registry = research.registry
        self.local = LocalFileProvider(research.settings.historical_sources_dir)
        self.alpaca = AlpacaProvider(research.settings)
        self.root = research.settings.historical_dir
        (self.root / "raw").mkdir(parents=True, exist_ok=True)
        (self.root / "sessions").mkdir(parents=True, exist_ok=True)

    def providers(self) -> Json:
        return {
            "providers": [
                {
                    "id": "local_file",
                    "configured": True,
                    "feeds": ["fabricated_l1"],
                    "coverage": "fabricated offline fixture",
                    "cost": "none",
                },
                {
                    "id": "alpaca",
                    "configured": bool(
                        self.research.settings.alpaca_key_id
                        and self.research.settings.alpaca_secret_key
                    ),
                    "feeds": ["iex", "sip"],
                    "coverage": "IEX single venue or SIP consolidated, subject to entitlement",
                    "cost": "check your Alpaca subscription before approving an import",
                },
            ]
        }

    def datasets(self) -> Json:
        return {"datasets": self.local.datasets()}

    def create(
        self,
        provider: str,
        symbol: str,
        feed: str,
        start: str | None,
        end: str | None,
        confirm_access: bool,
    ) -> Row:
        if provider == "local_file":
            if symbol != "AAPL" or feed != "fabricated_l1" or start or end:
                raise ServiceError(422, "UnknownLocalDataset", "select the bundled AAPL fixture")
        elif provider == "alpaca":
            if (
                feed not in ("iex", "sip")
                or not start
                or not end
                or _utc_instant(start) >= _utc_instant(end)
            ):
                raise ServiceError(422, "InvalidProviderRequest", "select a feed and UTC range")
            if not confirm_access:
                raise ServiceError(
                    409,
                    "AccessConfirmationRequired",
                    "confirm feed entitlement and any provider charges before import",
                )
            if (
                not self.research.settings.alpaca_key_id
                or not self.research.settings.alpaca_secret_key
            ):
                raise ServiceError(
                    503, "ProviderNotConfigured", "Alpaca data credentials are missing"
                )
        else:
            raise ServiceError(422, "UnsupportedProvider", "historical provider is not supported")
        request_key = hashlib.sha256(
            json.dumps([provider, symbol, feed, start, end], separators=(",", ":")).encode()
        ).hexdigest()
        existing = self.registry.historical_by_key(request_key)
        if existing:
            if existing["status"] != "failed":
                return existing
            self.registry.retire_failed_historical_key(request_key)
        import_id = "hist-" + secrets.token_hex(5)
        with self.research._reserved():
            inserted = self.registry.insert_historical(
                {
                    "id": import_id,
                    "request_key": request_key,
                    "provider": provider,
                    "feed": feed,
                    "symbol": symbol,
                    "start_time": start,
                    "end_time": end,
                    "status": "pending",
                    "owner_pid": os.getpid(),
                    "created_at": now(),
                }
            )
            if not inserted:
                winner = self.registry.historical_by_key(request_key)
                if winner is None:
                    raise ServiceError(409, "ImportConflict", "concurrent import changed")
                self.research._release()
                return winner
        self.research._start(self._execute, import_id)
        return self.require(import_id)

    def _execute(self, import_id: str) -> None:
        row = self.require(import_id)
        if row["status"] != "pending":
            return
        self.registry.update_historical(import_id, status="running", started_at=now())
        provider = self.local if row["provider"] == "local_file" else self.alpaca
        self._import(
            import_id,
            lambda: provider.acquire(
                row["symbol"],
                row["feed"],
                row["start_time"],
                row["end_time"],
                self.root / "raw" / import_id,
            ),
        )

    def _import(self, import_id: str, acquire: Callable[[], Acquisition]) -> None:
        try:
            acquisition = acquire()
            manifest = markettwin.import_historical_jsonl(
                acquisition.input_path, self.session_dir(import_id), acquisition.metadata
            )
            verdict = markettwin.verify_historical(self.session_dir(import_id))
            if not verdict["verified"]:
                raise ServiceError(
                    422, "HistoricalVerificationFailed", "native replay did not verify"
                )
            self.registry.update_historical(
                import_id, status="completed", finished_at=now(), manifest_json=manifest
            )
        except (ServiceError, markettwin.NativeRunError) as error:
            self.registry.update_historical(
                import_id,
                status="failed",
                finished_at=now(),
                error_code=getattr(error, "code", "HistoricalImportFailed"),
                error_message=str(error),
            )
        except Exception:
            self.registry.update_historical(
                import_id,
                status="failed",
                finished_at=now(),
                error_code="HistoricalImportFailed",
                error_message="historical import failed unexpectedly",
            )

    def import_prepared(
        self,
        provider: str,
        feed: str,
        symbol: str,
        start: str | None,
        end: str | None,
        identity: str,
        prepare: Callable[[Path], Acquisition],
    ) -> Row:
        """Import records another subsystem already holds, on the calling thread.

        ``prepare`` writes the normalizer input into a new raw directory. The same native
        import and verification as a provider import follow, so a live recording becomes
        an ordinary historical session. ``identity`` makes the call idempotent.
        """
        request_key = hashlib.sha256(
            json.dumps([provider, symbol, feed, identity], separators=(",", ":")).encode()
        ).hexdigest()
        existing = self.registry.historical_by_key(request_key)
        if existing:
            if existing["status"] == "completed":
                return existing
            self.registry.update_historical(
                existing["id"],
                status="failed",
                finished_at=now(),
                error_code=existing["error_code"] or "WorkerInterrupted",
                error_message=existing["error_message"] or "superseded by a later attempt",
            )
            self.registry.retire_failed_historical_key(request_key)
        import_id = "hist-" + secrets.token_hex(5)
        self.registry.insert_historical(
            {
                "id": import_id,
                "request_key": request_key,
                "provider": provider,
                "feed": feed,
                "symbol": symbol,
                "start_time": start,
                "end_time": end,
                "status": "running",
                "owner_pid": os.getpid(),
                "created_at": now(),
                "started_at": now(),
            }
        )
        self._import(import_id, lambda: prepare(self.root / "raw" / import_id))
        return self.require(import_id)

    def require(self, import_id: str) -> Row:
        if _HISTORICAL_ID.fullmatch(import_id) is None:
            raise ServiceError(404, "HistoricalImportNotFound", "historical import ID is unknown")
        row = self.registry.historical_import(import_id)
        if row is None:
            raise ServiceError(404, "HistoricalImportNotFound", "historical import ID is unknown")
        return row

    def list(self, limit: int, offset: int) -> Json:
        rows = self.registry.historical_imports(limit, offset)
        return {"imports": rows, "offset": offset, "count": len(rows)}

    def session_dir(self, import_id: str) -> Path:
        return self.root / "sessions" / import_id

    def session(self, import_id: str) -> Json:
        row = self.require(import_id)
        if row["status"] != "completed":
            raise ServiceError(409, "SessionNotReady", "historical import has no completed session")
        quality = json.loads((self.session_dir(import_id) / "quality_report.json").read_text())
        metrics = json.loads((self.session_dir(import_id) / "historical_metrics.json").read_text())
        return {
            **row,
            "quality": quality,
            "metrics": metrics,
            "observation_kind": observation_kind(row["provider"], row["feed"]),
        }

    def page(
        self, import_id: str, artifact: str, offset: int, limit: int, kind: str | None = None
    ) -> Json:
        self.session(import_id)
        if artifact not in ("historical_events.jsonl", "historical_samples.jsonl"):
            raise ServiceError(422, "UnsupportedArtifact", "historical artifact is not exposed")
        records: list[Json] = []
        with (self.session_dir(import_id) / artifact).open() as source:
            matched = 0
            for line in source:
                record = json.loads(line)
                if kind is not None and record.get("kind") != kind:
                    continue
                if matched < offset:
                    matched += 1
                    continue
                if len(records) == limit:
                    break
                records.append(record)
                matched += 1
        return {
            "session_id": import_id,
            "offset": offset,
            "records": records,
            "next_offset": offset + len(records) if len(records) == limit else None,
        }

    def verify(self, import_id: str) -> Json:
        self.session(import_id)
        return markettwin.verify_historical(self.session_dir(import_id))

    def comparison(self, run_id: str, session_id: str) -> Json:
        run = run_detail(self.research, self.research.require_run(run_id))
        if run["status"] != "completed" or run["metrics"] is None:
            raise ServiceError(409, "RunNotCompleted", "synthetic run has no completed metrics")
        session = self.session(session_id)
        synthetic = run["metrics"]["market"]
        historical = session["metrics"]
        return {
            "synthetic_run_id": run_id,
            "historical_session_id": session_id,
            "purpose": "diagnostic comparison, not calibration or evidence of market realism",
            "metrics": [
                {
                    "name": "trade_count",
                    "synthetic": synthetic["trade_count"],
                    "historical": historical["trade_count"],
                    "unit": "reported trades",
                    "compatible": True,
                    "note": "Compare only after considering duration and feed/venue scope.",
                },
                {
                    "name": "traded_quantity",
                    "synthetic": synthetic["volume"],
                    "historical": historical["traded_quantity"],
                    "unit": "reported quantity",
                    "compatible": False,
                    "note": (
                        "Synthetic shares and provider-reported size are different "
                        "data-generating processes."
                    ),
                },
                {
                    "name": "quoted_spread",
                    "synthetic": synthetic["mean_spread_milliticks"],
                    "historical": historical["mean_quoted_spread_micro_usd"],
                    "unit": "synthetic milliticks / historical micro-USD",
                    "compatible": False,
                    "note": "Units and venue scope differ; values must not be ranked directly.",
                },
                {
                    "name": "return_volatility",
                    "synthetic": synthetic["midpoint_return_volatility_ppb"],
                    "historical": historical["trade_to_trade_return_volatility_ppb"],
                    "unit": "parts per billion",
                    "compatible": False,
                    "note": (
                        "Synthetic sampled-midpoint returns and historical trade-to-trade "
                        "returns use different observations."
                    ),
                },
            ],
            "limitations": [
                "No historical executions or hypothetical fills are inferred.",
                "Numerical similarity does not establish synthetic calibration.",
                "Session duration, sampling, and consolidated-versus-venue coverage may differ.",
            ],
        }
