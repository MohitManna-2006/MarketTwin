"""Phase 5B calibration (ADR 0011) through the API, on fabricated sessions only."""

from __future__ import annotations

import json
from datetime import date
from pathlib import Path
from typing import Any

import markettwin
import pytest
from fastapi.testclient import TestClient

from markettwin_api.app import create_app
from markettwin_api.calibration_engine import SessionInput, admit
from markettwin_api.calibration_spec import (
    CalibrationSpec,
    candidate_config,
    deviations,
    draw_candidates,
    session_window,
    utc_text,
)
from markettwin_api.settings import Settings

from .conftest import CONFIGS, finished, wait

Json = dict[str, Any]


def spec(name: str) -> CalibrationSpec:
    return CalibrationSpec.model_validate(
        json.loads((CONFIGS / "calibration" / f"{name}.json").read_text())
    )


def test_the_committed_aapl_spec_is_exactly_the_adr() -> None:
    aapl = spec("aapl_iex")
    assert aapl.symbol == "AAPL"
    assert aapl.feed == "iex"
    assert deviations(aapl) == []
    assert deviations(spec("fixture_smoke"))  # the smoke spec is small on purpose


def test_windows_follow_new_york_daylight_time() -> None:
    aapl = spec("aapl_iex")
    start, end = session_window(aapl, date(2026, 10, 19))
    assert (utc_text(start), utc_text(end)) == ("2026-10-19T14:30:00Z", "2026-10-19T15:30:00Z")
    start, _ = session_window(aapl, date(2026, 12, 1))
    assert utc_text(start) == "2026-12-01T15:30:00Z"


def test_the_draw_is_seeded_distinct_and_valid() -> None:
    aapl = spec("aapl_iex")
    first, second = draw_candidates(aapl), draw_candidates(aapl)
    assert first == second
    assert len(first) == 256
    assert len({tuple(sorted(point.items())) for point in first}) == 256
    base = json.loads((CONFIGS / "baseline.json").read_text())
    for point in first[:16]:
        config = candidate_config(base, point, 25_000, 1, "check")
        markettwin.canonicalize_config(config)
        noise = [a for a in config["scenario"]["agents"] if a["policy"]["kind"] == "Noise"]
        assert {a["wakeup"]["interval_ns"] for a in noise} == {point["noise_interval_ns"]}
    unchanged = candidate_config(base, None, 25_000, 1, "check")
    assert unchanged["scenario"]["agents"] == base["scenario"]["agents"]
    assert unchanged["scenario"]["reference_price_ticks"] == 25_000


def test_the_quality_gate_rejects_a_one_sided_session(tmp_path: Path) -> None:
    raw = tmp_path / "raw.jsonl"
    raw.write_text(
        '{"kind":"quote","t":"2026-10-19T14:30:00Z","bp":"250.00","ap":"0","bs":100,"as":0}\n'
        '{"kind":"trade","t":"2026-10-19T14:31:00Z","p":"250.00","s":10}\n'
    )
    markettwin.import_historical_jsonl(
        raw,
        tmp_path / "session",
        {
            "symbol": "AAPL",
            "provider": "fixture",
            "feed": "fabricated_l1",
            "acquired_at": "2026-10-20T00:00:00Z",
        },
    )
    start = 1_792_420_200 * 10**9
    session = SessionInput(
        id="hist-0000000000",
        day="2026-10-19",
        role="train",
        directory=tmp_path / "session",
        start="2026-10-19T14:30:00Z",
        end="2026-10-19T15:30:00Z",
        start_ns=start,
        end_ns=start + 3600 * 10**9,
        canonical_sha256=None,
        gaps_over_five_minutes=0,
    )
    _, reasons = admit(session)
    assert "quotes are two-sided for less than 90% of the window" in reasons
    assert any(reason.startswith("undefined facts") for reason in reasons)


@pytest.fixture(scope="module")
def calibration_client(tmp_path_factory: pytest.TempPathFactory) -> Any:
    settings = Settings(
        workspace=tmp_path_factory.mktemp("calibration"),
        scenarios_dir=CONFIGS,
        calibration_workers=4,
    )
    with TestClient(create_app(settings)) as client:
        yield client


def fixture_ids(client: TestClient) -> list[str]:
    response = client.post("/api/v1/calibrations/fixture-sessions")
    assert response.status_code == 201, response.text
    body = response.json()
    assert body["observation_kind"] == "FABRICATED TEST DATA"
    ids: list[str] = body["session_ids"]
    return ids


def calibrate(client: TestClient, ids: list[str]) -> Json:
    response = client.post(
        "/api/v1/calibrations", json={"spec": "fixture_smoke", "session_ids": ids}
    )
    assert response.status_code == 202, response.text
    return wait(client, f"/api/v1/calibrations/{response.json()['id']}", finished, timeout=300)


def test_specs_are_listed_with_their_adr_deviations(calibration_client: TestClient) -> None:
    specs = {
        item["key"]: item
        for item in calibration_client.get("/api/v1/calibrations/specs").json()["specs"]
    }
    assert specs["aapl_iex"]["deviations_from_adr"] == []
    assert specs["aapl_iex"]["problems"] == []
    assert specs["aapl_iex"]["sessions_required"] == 10
    assert specs["fixture_smoke"]["deviations_from_adr"]


def test_requests_are_checked_before_anything_runs(calibration_client: TestClient) -> None:
    client = calibration_client
    ids = fixture_ids(client)
    assert fixture_ids(client) == ids  # fixture imports are idempotent
    post = client.post
    assert (
        post("/api/v1/calibrations", json={"spec": "nope", "session_ids": ids}).status_code == 404
    )
    short = post("/api/v1/calibrations", json={"spec": "fixture_smoke", "session_ids": ids[:9]})
    assert short.status_code == 422
    assert short.json()["error"]["code"] == "InvalidCalibrationSessions"
    # Fabricated sessions can never stand in for observed IEX data.
    iex = post("/api/v1/calibrations", json={"spec": "aapl_iex", "session_ids": ids})
    assert iex.status_code == 422
    assert "from alpaca" in iex.json()["error"]["message"]
    twice = post(
        "/api/v1/calibrations", json={"spec": "fixture_smoke", "session_ids": [ids[0]] * 10}
    )
    assert twice.status_code == 422


def test_fabricated_pipeline_publishes_a_verified_artifact(calibration_client: TestClient) -> None:
    client = calibration_client
    ids = fixture_ids(client)
    row = calibrate(client, ids)
    assert row["status"] == "completed", row
    assert row["observation_kind"] == "FABRICATED TEST DATA"
    assert [item["role"] for item in row["sessions"]] == ["train"] * 6 + ["validation"] * 2 + [
        "test"
    ] * 2
    again = client.post("/api/v1/calibrations", json={"spec": "fixture_smoke", "session_ids": ids})
    assert again.json()["id"] == row["id"]

    body = client.get(f"/api/v1/calibrations/{row['id']}/report").json()
    report = body["report"]
    assert report["outcome"] in ("fit_holds", "null_result")
    assert "FABRICATED" in report["label"]
    assert report["deviations_from_adr"]
    assert report["stage1"]["candidates"] == 5  # the baseline plus four draws
    assert all(session["admitted"] for session in report["sessions"])
    assert set(report["targets"]) == {"train", "validation", "test"}
    assert report["targets"]["train"]["sessions"] == 6
    assert len(body["observed"]) == 10
    assert body["units"]["spread_cents_p50"] == "1/1000 cent"
    if report["selected"] is not None:
        assert report["test"]["selected"]["seeds"] == 2
        config = client.get(f"/api/v1/calibrations/{row['id']}/selected-config").json()
        canonical, _ = markettwin.canonicalize_config(config)
        assert json.loads(canonical) == config
        assert config["scenario"]["reference_price_ticks"] == report["reference_price_cents"]

    stage1 = client.get(f"/api/v1/calibrations/{row['id']}/candidates?stage=stage1&limit=2").json()
    assert stage1["total"] == 5
    assert [line["candidate"] for line in stage1["candidates"]] == [0, 1]
    assert stage1["candidates"][0]["parameters"] is None
    assert len(stage1["candidates"][0]["seeds"]) == 2

    verdict = client.post(f"/api/v1/calibrations/{row['id']}/verify").json()
    assert verdict["verified"], verdict
    directory = Path(client.app.state.calibration.directory(row["id"]))  # type: ignore[attr-defined]
    assert sorted(path.name for path in directory.iterdir()) == [
        "candidates.jsonl",
        "manifest.json",
        "observed_facts.json",
        "report.json",
        "selected_config.json",
        "spec.json",
    ]

    # The same sessions and spec in a fresh workspace reach the same decision.
    with TestClient(
        create_app(Settings(workspace=directory.parent.parent / "second", scenarios_dir=CONFIGS))
    ) as other:
        repeat = calibrate(other, fixture_ids(other))
        second = other.get(f"/api/v1/calibrations/{repeat['id']}/report").json()["report"]
    assert second["selected"] == report["selected"]
    assert second["test"] == report["test"]
    assert second["stage1"]["fit_ppm"] == report["stage1"]["fit_ppm"]
    assert second["targets"] == report["targets"]


def test_startup_leaves_jobs_of_another_live_process_running(tmp_path: Path) -> None:
    import subprocess

    from markettwin_api.registry import Registry, now

    other = subprocess.Popen(["sleep", "30"])
    try:
        registry = Registry(tmp_path / "registry.sqlite3")
        for row_id, owner in (("cal-live000000", other.pid), ("cal-orphan0000", None)):
            registry.insert_calibration(
                {
                    "id": row_id,
                    "request_key": row_id,
                    "spec_key": "fixture_smoke",
                    "spec_digest": "d",
                    "title": "t",
                    "symbol": "AAPL",
                    "feed": "fabricated_l1",
                    "observation_kind": "FABRICATED TEST DATA",
                    "status": "running",
                    "created_at": now(),
                    "sessions_json": [],
                    "owner_pid": owner,
                }
            )
        registry.close()
        restarted = Registry(tmp_path / "registry.sqlite3")
        assert restarted.fail_interrupted() == 1
        live, orphan = (
            restarted.calibration("cal-live000000"),
            restarted.calibration("cal-orphan0000"),
        )
        assert live is not None and live["status"] == "running"
        assert orphan is not None and orphan["status"] == "failed"
        other.kill()
        other.wait()
        assert restarted.fail_interrupted() == 1
        assert restarted.calibration("cal-live000000")["status"] == "failed"  # type: ignore[index]
        restarted.close()
    finally:
        other.kill()
