"""Replay verification through the API, including what a mismatch reports."""

from __future__ import annotations

import json

from fastapi.testclient import TestClient

from markettwin_api.settings import Settings
from tests.conftest import Json, load_config, short, submit_run, wait

API = "/api/v1"


def replay(client: TestClient, run: str) -> Json:
    response = client.post(f"{API}/runs/{run}/replay")
    assert response.status_code == 202, response.text
    return wait(client, f"{API}/runs/{run}/replay", lambda body: body["status"] != "running")


def test_an_untouched_run_verifies(client: TestClient, baseline: Json) -> None:
    assert client.get(f"{API}/runs/{baseline['id']}/replay").json()["status"] == "not_checked"
    result = replay(client, baseline["id"])
    assert result["status"] == "verified"
    assert result["verified"] is True and result["code"] == "Verified"
    assert result["sequence"] is None and result["expected"] is None
    assert result["checked_at"] and result["processing_ms"] > 0
    assert client.get(f"{API}/runs/{baseline['id']}").json()["replay_status"] == "verified"


def test_a_changed_trade_is_reported_at_its_sequence_with_both_records(
    client: TestClient, settings: Settings
) -> None:
    run = submit_run(client, short(load_config("baseline"), 120, "tampered-trace"))
    trace = settings.runs_dir / run["id"] / "trace.jsonl"
    lines = trace.read_text().splitlines(keepends=True)
    position = next(index for index, line in enumerate(lines) if '"kind":"Trade"' in line)
    original = json.loads(lines[position])
    altered = lines[position].replace(
        f'"quantity":{original["payload"]["quantity"]}',
        f'"quantity":{original["payload"]["quantity"] + 1}',
        1,
    )
    assert altered != lines[position]
    lines[position] = altered
    trace.write_text("".join(lines))

    result = replay(client, run["id"])
    assert result["status"] == "mismatch"
    assert result["verified"] is False
    assert result["code"] == "TraceMismatch"
    assert result["message"] == "execution output differs"
    assert result["sequence"] == original["sequence"]
    assert result["expected"]["payload"]["quantity"] == original["payload"]["quantity"] + 1
    assert result["actual"] == original
    assert result["actual_raw"] == json.dumps(original, separators=(",", ":")) + "\n"
    # The command that caused the differing output is the last one verified.
    assert result["previous_command"]["sequence"] == original["causation_id"]
    assert result["previous_output"]["sequence"] < original["sequence"]


def test_a_changed_ledger_entry_is_an_accounting_mismatch(
    client: TestClient, settings: Settings
) -> None:
    run = submit_run(client, short(load_config("baseline"), 120, "tampered-ledger"))
    path = settings.runs_dir / run["id"] / "accounting.jsonl"
    lines = path.read_text().splitlines(keepends=True)
    position = next(index for index, line in enumerate(lines) if '"kind":"LedgerEntry"' in line)
    record = json.loads(lines[position])
    lines[position] = lines[position].replace('"fee_minor":0', '"fee_minor":1', 1)
    path.write_text("".join(lines))
    result = replay(client, run["id"])
    assert (result["status"], result["code"]) == ("mismatch", "AccountingMismatch")
    assert result["sequence"] == record["sequence"]
    assert result["actual"]["payload"] == record["payload"]


def test_a_recording_from_another_engine_version_is_unsupported_not_mismatched(
    client: TestClient, settings: Settings
) -> None:
    run = submit_run(client, short(load_config("baseline"), 30, "other-engine"))
    path = settings.runs_dir / run["id"] / "manifest.json"
    manifest = json.loads(path.read_text())
    manifest["engine_version"] = "0.3.0"
    path.write_text(json.dumps(manifest, separators=(",", ":")) + "\n")
    result = replay(client, run["id"])
    assert (result["status"], result["code"]) == ("unsupported_version", "UnsupportedVersion")
    assert result["verified"] is False


def test_missing_artifacts_are_an_error_with_the_engine_reason(
    client: TestClient, settings: Settings
) -> None:
    run = submit_run(client, short(load_config("baseline"), 30, "lost-manifest"))
    (settings.runs_dir / run["id"] / "manifest.json").unlink()
    result = replay(client, run["id"])
    assert (result["status"], result["code"]) == ("error", "IncompleteArtifacts")
    assert result["message"] == "completed manifest is missing"
