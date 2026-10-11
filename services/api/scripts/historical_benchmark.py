"""Offline, fabricated throughput smoke for the Phase 4A native pipeline."""

from __future__ import annotations

import argparse
import json
import resource
import sys
import tempfile
import time
from pathlib import Path

import markettwin

from markettwin_api.historical_service import HistoricalResearch
from markettwin_api.registry import now
from markettwin_api.service import Research
from markettwin_api.settings import Settings


def fixture(path: Path, records: int) -> None:
    with path.open("w", encoding="utf-8") as output:
        for index in range(records):
            timestamp = f"2024-03-11T13:30:00.{index:09d}Z"
            if index % 2:
                record = {
                    "kind": "trade",
                    "symbol": "AAPL",
                    "t": timestamp,
                    "p": "170.125",
                    "s": 10,
                    "i": index,
                    "x": "X",
                    "sequence": index,
                }
            else:
                record = {
                    "kind": "quote",
                    "symbol": "AAPL",
                    "t": timestamp,
                    "bp": "170.12",
                    "ap": "170.13",
                    "bs": 100,
                    "as": 120,
                    "bx": "X",
                    "ax": "X",
                    "sequence": index,
                }
            output.write(json.dumps(record, separators=(",", ":")) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--records", type=int, default=50_000)
    args = parser.parse_args()
    if not 1 <= args.records <= 500_000:
        parser.error("--records must be between 1 and 500000")
    with tempfile.TemporaryDirectory(prefix="markettwin-historical-benchmark-") as temporary:
        root = Path(temporary)
        source = root / "records.jsonl"
        research = Research(Settings(workspace=root / "workspace"))
        historical = HistoricalResearch(research)
        session = historical.session_dir("bench")
        fixture(source, args.records)
        started = time.perf_counter()
        manifest = markettwin.import_historical_jsonl(
            source,
            session,
            {
                "symbol": "AAPL",
                "provider": "benchmark_fixture",
                "feed": "fabricated_l1",
                "scope": "single_venue",
                "license": "fabricated_benchmark_only",
                "acquired_at": "2024-03-11T00:00:00Z",
            },
        )
        import_seconds = time.perf_counter() - started
        started = time.perf_counter()
        verdict = markettwin.verify_historical(session)
        verify_seconds = time.perf_counter() - started
        stamp = now()
        research.registry.insert_historical(
            {
                "id": "bench",
                "request_key": "fabricated-benchmark",
                "provider": "benchmark_fixture",
                "feed": "fabricated_l1",
                "symbol": "AAPL",
                "start_time": None,
                "end_time": None,
                "status": "completed",
                "created_at": stamp,
                "started_at": stamp,
                "finished_at": stamp,
                "manifest_json": manifest,
            }
        )
        started = time.perf_counter()
        for _ in range(100):
            historical.page("bench", "historical_samples.jsonl", 0, 500)
        series_response_ms = (time.perf_counter() - started) * 10
        artifacts = sum(path.stat().st_size for path in session.iterdir())
        maximum_rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        rss_bytes = maximum_rss if sys.platform == "darwin" else maximum_rss * 1024
        research.close()
        print(
            json.dumps(
                {
                    "records": args.records,
                    "import_seconds": round(import_seconds, 6),
                    "records_per_second": round(args.records / import_seconds),
                    "verify_seconds": round(verify_seconds, 6),
                    "verify_events_per_second": round(args.records / verify_seconds),
                    "artifact_bytes": artifacts,
                    "process_peak_rss_bytes": rss_bytes,
                    "mean_series_page_ms": round(series_response_ms, 6),
                    "verified": verdict["verified"],
                    "fixture": "fabricated",
                },
                separators=(",", ":"),
            )
        )


if __name__ == "__main__":
    main()
