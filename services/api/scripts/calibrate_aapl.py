"""Run the ADR 0011 calibration of AAPL against observed IEX sessions, end to end.

    uv run python scripts/calibrate_aapl.py --confirm-free-iex \
        --export ../../docs/experiments/phase5b-calibration

Needs ALPACA_API_KEY_ID and ALPACA_API_SECRET_KEY. It requests the IEX feed only, which
Alpaca's free plan includes, and never SIP. Without --dates it asks Alpaca's trading
calendar for the ten most recent full regular sessions (16:00 close) before today in New
York. Each window is imported through the ordinary Phase 4A path into the service
workspace, then the committed spec configs/calibration/aapl_iex.json runs on them.

--export copies the aggregate results that ADR 0011 allows in Git: the report, the
per-session facts, the spec, the selected config, and the manifest. Raw and normalized
observed records stay in the untracked workspace.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
import shutil
import sys
import time
from collections.abc import Callable
from datetime import date, datetime, timedelta
from pathlib import Path
from zoneinfo import ZoneInfo

import httpx

from markettwin_api.calibration_service import CalibrationResearch
from markettwin_api.calibration_spec import CalibrationSpec, session_window, utc_text
from markettwin_api.historical_service import HistoricalResearch
from markettwin_api.registry import Row
from markettwin_api.service import Research, ServiceError
from markettwin_api.settings import load_settings

SPEC = "aapl_iex"
EXPORTED = ("report.json", "observed_facts.json", "spec.json", "selected_config.json")


def recent_sessions(key_id: str, secret: str, count: int) -> list[date]:
    base = os.environ.get("ALPACA_TRADING_BASE_URL", "https://paper-api.alpaca.markets")
    today = datetime.now(ZoneInfo("America/New_York")).date()
    response = httpx.get(
        f"{base}/v2/calendar",
        params={
            "start": (today - timedelta(days=40)).isoformat(),
            "end": (today - timedelta(days=1)).isoformat(),
        },
        headers={"APCA-API-KEY-ID": key_id, "APCA-API-SECRET-KEY": secret},
        timeout=30,
    )
    if response.status_code != 200:
        raise SystemExit(f"Alpaca calendar request failed with HTTP {response.status_code}")
    full = [
        date.fromisoformat(day["date"])
        for day in response.json()
        if day["close"] == "16:00" and date.fromisoformat(day["date"]) < today
    ]
    if len(full) < count:
        raise SystemExit("the calendar returned fewer full sessions than the spec needs")
    return sorted(full)[-count:]


def settle(read: Callable[[str], Row], identifier: str, label: str) -> Row:
    last = ""
    while True:
        row = read(identifier)
        progress = row.get("progress") or {}
        line = f"{label}: {row['status']} {progress.get('phase', '')} "
        line += f"{progress.get('completed', '')}/{progress.get('total', '')}".strip("/")
        if line != last:
            print(line, flush=True)
            last = line
        if row["status"] in ("completed", "failed"):
            return row
        time.sleep(1.0)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--confirm-free-iex",
        action="store_true",
        help="confirm the IEX feed is within your Alpaca plan at no charge",
    )
    parser.add_argument("--dates", help="comma-separated YYYY-MM-DD trading days, oldest first")
    parser.add_argument("--export", type=Path, help="directory for the committed aggregates")
    arguments = parser.parse_args()
    if not arguments.confirm_free_iex:
        parser.error("pass --confirm-free-iex to approve ten one-hour IEX imports")

    settings = load_settings()
    if not settings.alpaca_key_id or not settings.alpaca_secret_key:
        parser.error("set ALPACA_API_KEY_ID and ALPACA_API_SECRET_KEY")
    if "MARKETTWIN_HISTORICAL_MAX_PAGES" not in os.environ:
        # An hour of AAPL IEX quotes can exceed the interactive default of 20 pages.
        settings = dataclasses.replace(settings, historical_max_pages=60)
    spec = CalibrationSpec.model_validate(
        json.loads((settings.calibration_specs_dir / f"{SPEC}.json").read_text())
    )
    if arguments.dates:
        days = [date.fromisoformat(item) for item in arguments.dates.split(",")]
    else:
        days = recent_sessions(settings.alpaca_key_id, settings.alpaca_secret_key, 10)
    if len(days) != spec.session_count:
        parser.error(f"the spec needs {spec.session_count} days")
    print("sessions:", ", ".join(day.isoformat() for day in days), flush=True)

    research = Research(settings)
    historical = HistoricalResearch(research)
    calibration = CalibrationResearch(research, historical)
    try:
        ids = []
        for day in days:
            start, end = session_window(spec, day)
            row = historical.create(
                "alpaca", spec.symbol, "iex", utc_text(start), utc_text(end), True
            )
            done = settle(historical.require, row["id"], f"import {day}")
            if done["status"] != "completed":
                print(f"import {day} failed: {done['error_code']}: {done['error_message']}")
                return 1
            ids.append(row["id"])
        created = calibration.create(SPEC, ids)
        done = settle(calibration.require, created["id"], f"calibration {created['id']}")
        if done["status"] != "completed":
            print(f"calibration failed: {done['error_code']}: {done['error_message']}")
            return 1
        report = calibration.report(created["id"])["report"]
        print(json.dumps({key: report[key] for key in ("outcome", "label", "null_reasons")}))
        print(json.dumps(report["acceptance"]))
        verdict = calibration.verify(created["id"])
        print("verified:", verdict["verified"])
        if arguments.export:
            arguments.export.mkdir(parents=True, exist_ok=True)
            source = calibration.directory(created["id"])
            for name in (*EXPORTED, "manifest.json"):
                shutil.copyfile(source / name, arguments.export / name)
            print("exported to", arguments.export)
        return 0 if verdict["verified"] else 1
    except ServiceError as error:
        print(f"{error.code}: {error.message}", file=sys.stderr)
        return 1
    finally:
        calibration.close()
        research.close()


if __name__ == "__main__":
    sys.exit(main())
