"""Timeline forks and the AI desk through the API (ADR 0012), on the real engine."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import markettwin
from fastapi.testclient import TestClient

from markettwin_api.ai_provider import ProviderError
from markettwin_api.app import create_app
from markettwin_api.desk import DeskDecider, DeskDecision, plain_rationale
from markettwin_api.settings import Settings

from .conftest import CONFIGS, SECOND, finished, load_config, short, submit_run, wait

Json = dict[str, Any]

DESK = {
    "scenario": "latency_shock",
    "agent_id": 3,
    "seed": 42,
    "horizon_ns": 300 * SECOND,
    "decision_interval_ns": 30 * SECOND,
    "source": "fixture",
}


def desk_run(client: TestClient) -> Json:
    response = client.post("/api/v1/desks", json=DESK)
    assert response.status_code == 202, response.text
    return wait(client, f"/api/v1/runs/{response.json()['id']}", finished)


def fork(client: TestClient, run_id: str, body: Json) -> Json:
    response = client.post(f"/api/v1/runs/{run_id}/forks", json=body)
    assert response.status_code == 202, response.text
    return wait(client, f"/api/v1/runs/{response.json()['id']}", finished)


def test_a_fork_shares_its_parents_past_and_reports_where_it_parts(client: TestClient) -> None:
    parent = submit_run(client, short(load_config("latency_shock"), 300), "latency_shock")
    assert parent["status"] == "completed"
    stress = {
        "kind": "QuoteWithdrawalStress",
        "at_ns": 120 * SECOND,
        "agents": [1, 2, 3],
    }
    child = fork(
        client,
        parent["id"],
        {"title": "makers pull quotes", "at_ns": 120 * SECOND, "add_shocks": [stress]},
    )
    assert child["status"] == "completed", child
    assert child["source_kind"] == "fork"
    assert child["parent_run_id"] == parent["id"]
    assert child["fork_status"] == "verified"
    report = client.get(f"/api/v1/runs/{child['id']}/divergence").json()
    assert report["verified"] is True
    body = report["report"]
    assert body["label"] == "ONE SAMPLE PATH"
    assert body["prefix"]["records"] > 0
    assert body["divergence"]["first"]["timestamp_ns"] >= 120 * SECOND
    assert body["divergence"]["first_market"] is not None

    # The same past, nothing changed: no divergence at all.
    empty = fork(client, parent["id"], {"at_ns": 100 * SECOND})
    quiet = client.get(f"/api/v1/runs/{empty['id']}/divergence").json()["report"]
    assert quiet["divergence"]["first"] is None
    assert all(
        value["difference"] == 0 for agent in quiet["agents"] for value in agent["values"].values()
    )

    tree = client.get(f"/api/v1/runs/{child['id']}/timeline").json()
    assert tree["root_id"] == parent["id"]
    assert {node["id"] for node in tree["nodes"]} >= {parent["id"], child["id"], empty["id"]}
    assert next(node for node in tree["nodes"] if node["id"] == child["id"])["title"] == (
        "makers pull quotes"
    )

    late = client.post(f"/api/v1/runs/{parent['id']}/forks", json={"at_ns": 900 * SECOND})
    assert late.status_code == 422
    assert late.json()["error"]["code"] == "InvalidFork"
    started = client.post(
        f"/api/v1/runs/{parent['id']}/forks",
        json={"at_ns": 10 * SECOND, "add_shocks": [{**stress, "at_ns": 5 * SECOND}]},
    )
    assert started.status_code == 422
    blocked = client.delete(f"/api/v1/runs/{parent['id']}")
    assert blocked.status_code == 409
    assert blocked.json()["error"]["code"] == "RunHasForks"

    repeat = client.post(f"/api/v1/runs/{child['id']}/repeat", json={"seeds": 3})
    assert repeat.status_code == 202, repeat.text
    experiment = wait(client, f"/api/v1/experiments/{repeat.json()['id']}", finished, 300)
    assert experiment["status"] == "completed", experiment


def test_an_ai_desk_records_decisions_and_its_ledger_forks_each_one(client: TestClient) -> None:
    estimate = client.post("/api/v1/desks/estimate", json=DESK).json()
    assert estimate == {
        "model": "fixture",
        "source": "fixture",
        "decisions": 10,
        "model_calls": 0,
        "model_configured": False,
    }
    run = desk_run(client)
    assert run["status"] == "completed", run
    assert run["desk"]["source"] == "fixture"
    listed = client.get(f"/api/v1/runs/{run['id']}/decisions").json()
    decisions = listed["decisions"]
    assert [item["at_ns"] for item in decisions] == [30 * SECOND * k for k in range(1, 11)]
    assert {item["source"] for item in decisions} == {"model"}
    assert {item["model"] for item in decisions} == {"fixture"}
    assert all("fundamental" not in json.dumps(item["observation"]) for item in decisions)
    replay = client.post(f"/api/v1/runs/{run['id']}/replay")
    assert replay.status_code in (200, 202)
    checked = wait(
        client,
        f"/api/v1/runs/{run['id']}/replay",
        lambda body: body["status"] not in ("running", "not_checked"),
    )
    assert checked["status"] == "verified", checked

    held = fork(client, run["id"], {"at_ns": 90 * SECOND, "decisions": "hold"})
    assert held["fork_status"] == "verified"
    assert held["desk"]["source"] == "recorded"
    sources = [
        item["source"]
        for item in client.get(f"/api/v1/runs/{held['id']}/decisions").json()["decisions"]
    ]
    assert sources[:2] == ["model", "model"] and set(sources[2:]) == {"hold"}

    started = client.post(f"/api/v1/runs/{run['id']}/decision-ledger")
    assert started.status_code == 202
    ledger = wait(
        client,
        f"/api/v1/runs/{run['id']}/decision-ledger",
        lambda body: body["ledger"]["status"] not in ("pending", "running"),
        300,
    )["ledger"]
    assert ledger["status"] == "completed", ledger
    assert len(ledger["entries"]) == 10
    changed = [
        entry for entry in ledger["entries"] if entry["status"] not in ("unchanged", "at_horizon")
    ]
    assert ledger["entries"][-1]["status"] == "at_horizon"
    assert changed, "the fixture changes its directive at least once in five minutes"
    for entry in changed:
        assert entry["status"] == "completed"
        assert isinstance(entry["value_minor"], int)
        assert entry["fork_run_id"]
    assert ledger["versus_hold"]["status"] == "completed"
    tree = client.get(f"/api/v1/runs/{run['id']}/timeline").json()
    assert all(node["source_kind"] != "ledger" for node in tree["nodes"])

    repeat = client.post(f"/api/v1/runs/{run['id']}/repeat", json={"seeds": 2})
    assert repeat.status_code == 202, repeat.text
    experiment = wait(client, f"/api/v1/experiments/{repeat.json()['id']}", finished, 300)
    assert experiment["status"] == "completed", experiment


def test_a_model_desk_needs_a_configured_model_and_an_exact_approval(tmp_path: Path) -> None:
    with TestClient(create_app(Settings(workspace=tmp_path, scenarios_dir=CONFIGS))) as client:
        refused = client.post("/api/v1/desks", json={**DESK, "source": "model"})
        assert refused.status_code == 422
        assert refused.json()["error"]["code"] == "ProviderNotConfigured"
    configured = Settings(
        workspace=tmp_path / "configured", scenarios_dir=CONFIGS, ai_model="m", ai_api_key="k"
    )
    with TestClient(create_app(configured)) as client:
        unapproved = client.post("/api/v1/desks", json={**DESK, "source": "model"})
        assert unapproved.status_code == 409
        assert unapproved.json()["error"]["details"] == {"model_calls": 10}


class ScriptedProvider:
    """Answers like a model would, without a network: widen once, then fail once."""

    def __init__(self) -> None:
        self.last_usage: dict[str, Any] = {}
        self.last_cost_usd: str | None = None
        self.calls = 0

    def structured(self, model_type: type[DeskDecision], system: str, data: Json) -> Any:
        self.calls += 1
        assert "synthetic" in system
        assert "fundamental" not in json.dumps(data["observation"])
        if self.calls == 2:
            raise ProviderError("ProviderUnavailable", "down")
        self.last_usage = {"prompt_tokens": 120, "completion_tokens": 30}
        self.last_cost_usd = "0.0001"
        current = dict(data["directive_in_force"])
        current["half_spread_ticks"] = 3
        current["rationale"] = "widen — spreads look thin\nnext line"
        return model_type.model_validate(current)


def test_the_desk_decider_records_usage_and_survives_a_failed_call(tmp_path: Path) -> None:
    config = json.loads((CONFIGS / "latency_shock.json").read_text())
    config.update(schema_version=4, run_id="decider", horizon_ns=120 * SECOND, forks=[])
    config["accounting"] = {
        "fees": {"maker_ppm": 0, "taker_ppm": 0},
        "default_initial_cash_minor": 0,
        "accounts": [],
    }
    agent = config["scenario"]["agents"][2]
    agent["policy"] = {
        "kind": "ModelDirected",
        "initial": {
            "mode": "quote",
            "half_spread_ticks": 2,
            "skew_ticks": 0,
            "quote_size": 100,
            "levels": 3,
            "max_position": 500,
            "rationale": "start",
        },
        "decision_interval_ns": 30 * SECOND,
        "max_half_spread_ticks": 10,
        "max_skew_ticks": 5,
        "mandate": "Make markets.",
    }
    provider = ScriptedProvider()
    decider = DeskDecider(provider, "scripted")
    markettwin.run(config, tmp_path / "run", decide=decider)
    assert decider.usage() == {
        "calls": 4,
        "failures": 1,
        "input_tokens": 360,
        "output_tokens": 90,
        "cost_usd": "0.0003",
    }
    records = [
        json.loads(line)["payload"]
        for line in (tmp_path / "run" / "trace.jsonl").read_text().splitlines()
        if '"kind":"Decision"' in line
    ]
    assert [item["source"] for item in records] == ["model", "fallback", "model", "model"]
    assert records[1]["error_code"] == "ProviderUnavailable"
    assert records[0]["directive"]["rationale"] == "widen spreads look thin next line"
    assert records[0]["model"] == "scripted"
    assert markettwin.verify_replay(tmp_path / "run")["verified"] is True
    assert plain_rationale("a" * 400) == "a" * 280


class _Usage:
    input_tokens = 900
    output_tokens = 60


class _Text:
    type = "text"

    def __init__(self, text: str) -> None:
        self.text = text


class _Response:
    def __init__(self, text: str, stop_reason: str = "end_turn") -> None:
        self.content = [_Text(text)]
        self.stop_reason = stop_reason
        self.usage = _Usage()


def test_the_anthropic_provider_asks_for_a_structured_directive_at_low_effort(
    tmp_path: Path,
) -> None:
    from markettwin_api.desk import DESK_SCHEMA, AnthropicDeskProvider, model_configured

    settings = Settings(
        workspace=tmp_path,
        ai_provider="anthropic",
        ai_model="claude-haiku-5-5",
        ai_api_key="test-key",
        ai_input_usd_per_million_tokens="0.10",
        ai_output_usd_per_million_tokens="0.50",
    )
    assert model_configured(settings)
    provider = AnthropicDeskProvider(settings)
    sent: list[Json] = []
    answer = json.dumps(
        {
            "mode": "quote",
            "half_spread_ticks": 3,
            "skew_ticks": -1,
            "quote_size": 50,
            "levels": 2,
            "max_position": 400,
            "rationale": "widen against a long book",
        }
    )

    def create(**request: Any) -> _Response:
        sent.append(request)
        return _Response(answer)

    provider.client.messages.create = create  # type: ignore[method-assign]
    decision = provider.structured(DeskDecision, "system", {"observation": {}})
    assert decision.half_spread_ticks == 3
    assert sent[0]["model"] == "claude-haiku-5-5"
    assert sent[0]["output_config"] == {
        "effort": "low",
        "format": {"type": "json_schema", "schema": DESK_SCHEMA},
    }
    assert provider.last_usage == {"prompt_tokens": 900, "completion_tokens": 60}
    assert provider.last_cost_usd == "0.00012"

    provider.client.messages.create = (  # type: ignore[method-assign]
        lambda **request: _Response("", "refusal")
    )
    try:
        provider.structured(DeskDecision, "system", {})
    except ProviderError as error:
        assert error.code == "ModelRefused"
    else:
        raise AssertionError("a refusal must fail the decision")
    assert not model_configured(Settings(workspace=tmp_path, ai_provider="anthropic"))
