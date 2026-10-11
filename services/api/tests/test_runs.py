"""Runs through the HTTP API: submission, recorded telemetry, and its time semantics."""

from __future__ import annotations

import json
import time
from pathlib import Path

from fastapi.testclient import TestClient

from markettwin_api.app import create_app
from markettwin_api.registry import Registry, now
from markettwin_api.settings import Settings
from tests.conftest import CONFIGS, SECOND, Json, load_config, short, submit_run

API = "/api/v1"


def get(client: TestClient, path: str, **params: object) -> Json:
    response = client.get(f"{API}{path}", params=params)
    assert response.status_code == 200, response.text
    body: Json = response.json()
    return body


def test_health_names_the_engine_and_the_service_limits(client: TestClient) -> None:
    health = get(client, "/health")
    assert health["status"] == "ok"
    assert health["engine_version"] == "0.4.0"
    assert health["counts"]["scenarios"] == 6
    assert health["limits"]["max_horizon_ns"] == 24 * 3600 * SECOND


def test_scenarios_are_the_shipped_configs_and_nothing_else(client: TestClient) -> None:
    scenarios = get(client, "/scenarios")
    assert [item["key"] for item in scenarios] == [
        "baseline",
        "fundamental_shock",
        "liquidity_shock",
        "volatility_shock",
        "baseline_accounting",
        "phase1b_demo",
    ]
    by_key = {item["key"]: item for item in scenarios}
    assert by_key["baseline"]["policies"] == {
        "MarketMaker": 2,
        "InventoryMarketMaker": 1,
        "Value": 2,
        "Momentum": 2,
        "Noise": 4,
    }
    assert by_key["liquidity_shock"]["shocks"] == ["LiquidityWithdrawal"]
    assert by_key["phase1b_demo"]["kind"] == "scripted"
    detail = get(client, "/scenarios/fundamental_shock")
    assert detail["config"] == load_config("fundamental_shock")
    assert client.get(f"{API}/scenarios/../../etc/passwd").status_code == 404
    assert client.get(f"{API}/scenarios/missing").json()["error"]["code"] == "ScenarioNotFound"


def test_the_field_catalogue_describes_exactly_the_native_policy_parameters(
    client: TestClient,
) -> None:
    catalogue = get(client, "/schema/fields")
    policies = {policy["kind"]: policy for policy in catalogue["policies"]}
    baseline = load_config("baseline")
    for agent in baseline["scenario"]["agents"]:
        policy = policies[agent["policy"]["kind"]]
        assert {field["name"] for field in policy["fields"]} == set(agent["policy"]) - {"kind"}
    levels = next(f for f in policies["MarketMaker"]["fields"] if f["name"] == "levels")
    assert (levels["minimum"], levels["maximum"], levels["unit"]) == (1, 32, "levels")
    assert policies["Noise"]["template"]["policy"]["kind"] == "Noise"
    assert [shock["kind"] for shock in catalogue["shocks"]] == [
        "FundamentalJump",
        "LiquidityWithdrawal",
        "VolatilityRegime",
    ]
    distribution = next(f for f in catalogue["wakeup"] if f["name"] == "distribution")
    assert distribution["options"] == ["Fixed", "Exponential"]


def test_validation_separates_schema_service_and_engine_errors(client: TestClient) -> None:
    def validate(config: object) -> Json:
        body: Json = client.post(f"{API}/configs/validate", json={"config": config}).json()
        return body

    ok = validate(load_config("baseline"))
    assert ok["valid"] and ok["errors"] == []
    assert ok["config_digest"] == "dd21c57970f4aa72f2e66bbc90f7095dacc69239b0f113e6f55d823caf02c9b4"
    assert ok["complexity"]["samples"] == 3600
    assert ok["complexity"]["fundamental_updates"] == 3600
    assert ok["complexity"]["wakeups_exact"] is False  # exponential schedules are expectations

    structural = load_config("baseline")
    structural["scenario"]["agents"][2]["policy"]["levels"] = 99
    structural["scenario"]["agents"][0]["limits"]["max_position"] = 1.5
    del structural["scenario"]["fundamental"]["mean_reversion_ppm"]
    errors = {error["path"]: error for error in validate(structural)["errors"]}
    assert set(errors) == {
        "scenario.agents.2.policy.levels",
        "scenario.agents.0.limits.max_position",
        "scenario.fundamental.mean_reversion_ppm",
    }
    assert all(error["source"] == "schema" for error in errors.values())

    # A rule relating two fields is the engine's, and comes back verbatim with no path.
    relational = load_config("baseline")
    relational["scenario"]["agents"][2]["policy"]["levels"] = 9
    (engine,) = validate(relational)["errors"]
    assert engine == {
        "source": "engine",
        "code": "InvalidConfig",
        "path": "",
        "message": "maker levels must fit twice within max_open_orders",
    }

    too_long = load_config("baseline") | {"horizon_ns": 25 * 3600 * SECOND}
    (limit,) = validate(too_long)["errors"]
    assert (limit["source"], limit["path"]) == ("api", "horizon_ns")

    assert validate("not a config")["valid"] is False
    assert validate({"schema_version": 7})["errors"][0]["source"] == "schema"


def test_a_submitted_run_is_the_native_run_with_its_provenance(
    client: TestClient, baseline: Json, settings: Settings
) -> None:
    assert baseline["status"] == "completed"
    assert baseline["error"] is None
    assert baseline["scenario_key"] == "baseline"
    assert (baseline["seed"], baseline["trade_count"], baseline["event_count"]) == (
        42,
        8205,
        298798,
    )
    assert baseline["final_time_ns"] == baseline["horizon_ns"] == 3600 * SECOND
    assert baseline["processing_ms"] > 0
    assert baseline["replay"]["status"] == "not_checked"

    directory = settings.runs_dir / baseline["id"]
    manifest = json.loads((directory / "manifest.json").read_text())
    assert baseline["manifest"] == manifest
    assert baseline["summary"] == manifest["summary"]
    assert baseline["metrics"] == json.loads((directory / "metrics.json").read_text())
    assert baseline["config"] == json.loads((directory / "config.json").read_text())
    assert baseline["units"]["tick_size_minor"] == 1
    assert {item["name"]: item["sha256"] for item in baseline["files"] if item["sha256"]} == (
        manifest["artifacts"]
    )
    assert baseline["availability"] == {
        "artifacts": True,
        "samples": True,
        "books": True,
        "fundamental": True,
        "agents": True,
        "trades": True,
        "index_error": None,
    }
    maker = next(agent for agent in baseline["agents"] if agent["id"] == 3)
    assert maker["policy"] == "InventoryMarketMaker"
    assert maker["config"]["limits"]["max_position"] == 2000
    assert maker["report"]["total_pnl_minor"] == -57308
    listed = get(client, "/runs")
    assert baseline["id"] in [run["id"] for run in listed["runs"]]


def test_series_are_the_recorded_samples_column_by_column(
    client: TestClient, baseline: Json, settings: Settings
) -> None:
    run = baseline["id"]
    recorded = []
    with (settings.runs_dir / run / "accounting.jsonl").open() as stream:
        for line in stream:
            record = json.loads(line)
            if record["kind"] == "Sample":
                recorded.append(record)
    assert len(recorded) == 3600

    times = get(client, f"/runs/{run}/samples/times")
    assert times["count"] == 3600
    assert times["timestamp_ns"] == [record["timestamp_ns"] for record in recorded]

    market = get(client, f"/runs/{run}/series/market")
    assert market["decimated"] is False and market["index"] == list(range(3600))
    columns = market["columns"]
    assert market["derived"] == ["mid_twice_ticks"]
    for position in (0, 1799, 3599):
        payload = recorded[position]["payload"]
        assert columns["bid_price_ticks"][position] == payload["bid"][0]
        assert columns["ask_quantity"][position] == payload["ask"][1]
        assert columns["spread_ticks"][position] == payload["spread_ticks"]
        assert columns["mark_twice_ticks"][position] == payload["mark_twice_ticks"]
        assert columns["fundamental_milliticks"][position] == payload["fundamental_milliticks"]
        assert columns["volume"][position] == payload["volume"]
        assert market["mark_sources"][columns["mark_source"][position]] == payload["mark_source"]
        assert columns["mid_twice_ticks"][position] == payload["bid"][0] + payload["ask"][0]

    agent = get(client, f"/runs/{run}/series/agents/3")
    last = recorded[-1]["payload"]["agents"][2]
    names = ("cash_minor", "position", "cost_basis_minor", "realized_pnl_minor")
    assert [agent["columns"][name][-1] for name in names] == last[:4]
    assert agent["columns"]["equity_minor"][-1] == last[5]

    # The derived series reproduce what the core reports as run totals.
    report = next(item for item in baseline["agents"] if item["id"] == 3)["report"]
    assert set(agent["derived"]) == {"drawdown_minor", "cum_bought", "cum_sold"}
    assert max(agent["columns"]["drawdown_minor"]) == report["max_drawdown_minor"]
    assert agent["columns"]["cum_bought"][-1] == report["bought_quantity"]
    assert agent["columns"]["cum_sold"][-1] == report["sold_quantity"]
    mean_abs = sum(abs(value) for value in agent["columns"]["position"]) * 1000 / 3600
    assert abs(mean_abs - report["mean_abs_position_millishares"]) <= 0.5

    assert client.get(f"{API}/runs/{run}/series/agents/99").status_code == 404


def test_a_time_resolves_to_the_latest_recorded_observation_never_between_two(
    client: TestClient, baseline: Json, settings: Settings
) -> None:
    run = baseline["id"]
    snapshots = {}
    with (settings.runs_dir / run / "trace.jsonl").open() as stream:
        for line in stream:
            if '"kind":"BookSnapshot"' in line:
                record = json.loads(line)
                snapshots[record["timestamp_ns"]] = record
    # Before the first snapshot there is no book to show.
    assert get_or_none(client, f"/runs/{run}/book", time_ns=999_999_999) is None
    # Exactly on a snapshot, and anywhere up to the next one, it is that snapshot.
    for asked, recorded_at in (
        (1 * SECOND, 1 * SECOND),
        (1 * SECOND + 1, 1 * SECOND),
        (1801 * SECOND - 1, 1800 * SECOND),
        (3600 * SECOND, 3600 * SECOND),
        (9999 * SECOND, 3600 * SECOND),
    ):
        book = get(client, f"/runs/{run}/book", time_ns=asked)
        payload = snapshots[recorded_at]["payload"]
        assert book["timestamp_ns"] == recorded_at
        assert book["sequence"] == snapshots[recorded_at]["sequence"]
        assert book["bids"] == [
            [level["price_ticks"], level["quantity"], level["order_count"]]
            for level in payload["bids"]
        ]
        assert book["asks"][0][0] == payload["best_ask_ticks"]

    window = get(client, f"/runs/{run}/samples", start=1798, count=4, agents="3,5")
    assert (window["start"], window["count"]) == (1798, 4)
    assert window["columns"]["timestamp_ns"] == [second * SECOND for second in range(1799, 1803)]
    assert set(window["agents"]) == {"3", "5"}
    for offset, book in enumerate(window["books"]):
        assert book["timestamp_ns"] == window["columns"]["timestamp_ns"][offset]
        assert book["bids"][0][0] == window["columns"]["bid_price_ticks"][offset]
        assert book["bids"][0][1] == window["columns"]["bid_quantity"][offset]
        assert book["asks"][0][0] == window["columns"]["ask_price_ticks"][offset]
    tail = get(client, f"/runs/{run}/samples", start=3598, count=512, agents="none")
    assert tail["count"] == 2 and tail["agents"] == {}
    assert client.get(f"{API}/runs/{run}/samples", params={"count": 5000}).status_code == 422


def get_or_none(client: TestClient, path: str, **params: object) -> object:
    response = client.get(f"{API}{path}", params=params)
    assert response.status_code == 200, response.text
    return response.json()


def test_decimation_plots_only_recorded_points_and_keeps_every_extreme(
    client: TestClient, baseline: Json
) -> None:
    run = baseline["id"]
    full = get(client, f"/runs/{run}/series/market")["columns"]
    reduced = get(client, f"/runs/{run}/series/market", max_points=200)
    assert reduced["decimated"] is True
    assert reduced["decimation"] == "min_max_per_bucket(mid_twice_ticks)"
    assert reduced["sample_count"] == 3600
    assert len(reduced["index"]) <= 200
    assert reduced["index"][0] == 0 and reduced["index"][-1] == 3599
    for name, column in reduced["columns"].items():
        assert column == [full[name][index] for index in reduced["index"]], name
    assert min(reduced["columns"]["mid_twice_ticks"]) == min(full["mid_twice_ticks"])
    assert max(reduced["columns"]["mid_twice_ticks"]) == max(full["mid_twice_ticks"])


def test_trades_and_ledger_pages_agree_with_each_other_and_with_the_metrics(
    client: TestClient, baseline: Json
) -> None:
    run = baseline["id"]
    first = get(client, f"/runs/{run}/trades", limit=3)
    assert first["total"] == baseline["trade_count"] == 8205
    assert [row["trade_id"] for row in first["rows"]] == [1, 2, 3]
    assert first["rows"][0] == {
        "timestamp_ns": 1_002_304_274,
        "sequence": 63,
        "trade_id": 1,
        "price_ticks": 15003,
        "quantity": 14,
        "maker_agent_id": 1,
        "taker_agent_id": 9,
        "aggressor_side": "Buy",
        "buy_order_id": 38_654_705_665,
        "sell_order_id": 4_294_967_301,
    }
    # The count of trades in a time window is the difference of the recorded counters.
    counts = get(client, f"/runs/{run}/series/market")["columns"]["trade_count"]
    windowed = get(
        client, f"/runs/{run}/trades", from_ns=600 * SECOND + 1, to_ns=1200 * SECOND, limit=1
    )
    assert windowed["total"] == counts[1199] - counts[599]

    report = next(item for item in baseline["agents"] if item["id"] == 3)["report"]
    ledger = get(client, f"/runs/{run}/agents/3/ledger", limit=1000)
    assert ledger["total"] == report["fills"] == 2627
    assert get(client, f"/runs/{run}/trades", agent_id=3, limit=1)["total"] == 2627
    final = get(client, f"/runs/{run}/agents/3/ledger", offset=2626, limit=5)["rows"]
    assert len(final) == 1
    assert final[0]["cash_minor"] == report["cash_minor"]
    assert final[0]["position"] == report["position"]
    assert final[0]["cost_basis_minor"] == report["cost_basis_minor"]
    assert final[0]["realized_pnl_minor"] == report["realized_pnl_minor"]
    assert {row["role"] for row in ledger["rows"]} == {"Maker"}  # a maker never takes
    bought = sum(row["quantity"] for row in page_all(client, run, 3) if row["side"] == "Buy")
    assert bought == report["bought_quantity"]


def page_all(client: TestClient, run: str, agent: int) -> list[Json]:
    rows: list[Json] = []
    while True:
        page = get(client, f"/runs/{run}/agents/{agent}/ledger", offset=len(rows), limit=1000)
        rows.extend(page["rows"])
        if len(rows) >= page["total"]:
            return rows


def test_records_page_in_sequence_order_and_can_be_positioned_and_filtered(
    client: TestClient, baseline: Json
) -> None:
    run = baseline["id"]
    head = get(client, f"/runs/{run}/events", limit=4)
    assert head["total"] == 46444 + 66455  # every command and outcome, without the headers
    assert [row["sequence"] for row in head["rows"]] == [1, 2, 3, 4]
    assert [row["channel"] for row in head["rows"]] == ["commands", "execution"] * 2
    assert head["rows"][0]["kind"] == "OrderArrival"
    assert head["rows"][0]["price_ticks"] == 14998 and head["rows"][0]["side"] == "Buy"
    assert head["rows"][1]["causation_id"] == 1 and head["rows"][1]["state"] == "Resting"

    at = get(client, f"/runs/{run}/events", at_or_before_ns=1800 * SECOND, limit=50)
    assert at["rows"][-1]["timestamp_ns"] <= 1800 * SECOND
    assert at["anchor"] == at["offset"] + len(at["rows"]) - 1
    following = get(client, f"/runs/{run}/events", offset=at["anchor"] + 1, limit=1)
    assert following["rows"][0]["timestamp_ns"] > 1800 * SECOND

    by_sequence = get(client, f"/runs/{run}/events", at_or_before_sequence=4281, limit=1)
    assert by_sequence["rows"][0]["sequence"] == 4281

    trades = get(client, f"/runs/{run}/events", kind="Trade", limit=1)
    assert trades["total"] == 8205
    mine = get(client, f"/runs/{run}/events", kind="Trade", agent_id=3, limit=1)
    assert mine["total"] == 2627  # as maker: the agent is named in the payload, not as actor
    commands = get(client, f"/runs/{run}/events", channel="commands", limit=1)
    assert commands["total"] == 46444
    order = head["rows"][0]["order_id"]
    lifecycle = get(client, f"/runs/{run}/events", order_id=order, limit=50)
    assert {row["order_id"] for row in lifecycle["rows"]} == {order}
    assert lifecycle["rows"][0]["kind"] == "OrderArrival"

    accounting = get(
        client, f"/runs/{run}/events", stream="accounting", kind="LedgerEntry", limit=2
    )
    assert accounting["total"] == 8205
    assert accounting["rows"][0]["channel"] == "accounting"
    assert accounting["rows"][0]["payload"]["trade_id"] == 1
    assert client.get(f"{API}/runs/{run}/events", params={"limit": 5000}).status_code == 422


def test_shock_transitions_are_the_recorded_ones(client: TestClient, liquidity: Json) -> None:
    (shock,) = liquidity["shocks"]
    assert shock["kind"] == "LiquidityWithdrawal"
    assert shock["definition"]["agents"] == [1, 2]
    assert [(item["phase"], item["timestamp_ns"]) for item in shock["transitions"]] == [
        ("Begin", 1200 * SECOND),
        ("End", 1800 * SECOND),
    ]
    recorded = get(client, f"/runs/{liquidity['id']}/events", kind="ScenarioShock", limit=10)
    assert [row["sequence"] for row in recorded["rows"]] == [
        item["sequence"] for item in shock["transitions"]
    ]


def test_a_scripted_run_shows_what_exists_and_labels_what_does_not(client: TestClient) -> None:
    run = submit_run(client, load_config("phase1b_demo"), "phase1b_demo")
    assert run["status"] == "completed"
    assert run["availability"]["fundamental"] is False
    assert run["availability"]["agents"] is False
    assert run["availability"]["samples"] is True  # the script takes two snapshots
    assert run["sample_count"] == 2 and run["sampling"] is None
    assert [agent["policy"] for agent in run["agents"]] == [None, None, None]
    market = get(client, f"/runs/{run['id']}/series/market")["columns"]
    assert market["fundamental_milliticks"] == [None, None]
    assert run["shocks"] == []


def test_a_failed_run_reports_the_engine_error_and_serves_no_artifacts(
    client: TestClient, settings: Settings
) -> None:
    starved = short(load_config("baseline"), 60, "starved") | {"max_trace_bytes": 4096}
    run = submit_run(client, starved)
    assert run["status"] == "failed"
    assert run["error"]["code"] == "ResourceLimit"
    assert run["error"]["message"] == "canonical trace exceeds byte budget"
    assert run["error"]["sequence"] > 0
    assert run["manifest"] is None and run["metrics"] is None
    assert run["availability"]["artifacts"] is False
    assert not (settings.runs_dir / run["id"]).exists()
    refused = client.get(f"{API}/runs/{run['id']}/series/market")
    assert refused.status_code == 409
    assert refused.json()["error"]["code"] == "RunNotCompleted"
    assert client.post(f"{API}/runs/{run['id']}/replay").status_code == 409


def test_an_invalid_config_is_refused_with_its_errors_and_creates_nothing(
    client: TestClient,
) -> None:
    before = get(client, "/runs")["total"]
    config = load_config("baseline")
    config["scenario"]["reference_price_ticks"] = 0
    response = client.post(f"{API}/runs", json={"config": config})
    assert response.status_code == 422
    error = response.json()["error"]
    assert error["code"] == "InvalidConfig"
    assert error["details"][0]["path"] == "scenario.reference_price_ticks"
    assert (
        client.post(f"{API}/runs", json={"config": config, "scenario_key": "x"}).status_code == 422
    )
    unknown = client.post(
        f"{API}/runs", json={"config": load_config("baseline"), "scenario_key": "x"}
    )
    assert unknown.json()["error"]["code"] == "UnknownScenario"
    assert get(client, "/runs")["total"] == before


def test_requests_cannot_reach_outside_the_registry(
    client: TestClient, baseline: Json, settings: Settings
) -> None:
    run = baseline["id"]
    for path in (
        f"/runs/{run}/artifacts/..%2F..%2Fregistry.sqlite3",
        f"/runs/{run}/artifacts/registry.sqlite3",
        f"/runs/{run}/artifacts/trace.jsonl.partial",
        "/runs/..%2F..%2Fetc/artifacts/manifest.json",
        "/runs/nope",
        "/runs/nope/series/market",
    ):
        response = client.get(f"{API}{path}")
        assert response.status_code == 404, path
    manifest = client.get(f"{API}/runs/{run}/artifacts/manifest.json")
    assert manifest.content == (settings.runs_dir / run / "manifest.json").read_bytes()
    assert "attachment" in manifest.headers["content-disposition"]

    oversized = json.dumps({"config": {"padding": "x" * (settings.max_request_bytes + 1)}})
    too_big = client.post(
        f"{API}/configs/validate", content=oversized, headers={"content-type": "application/json"}
    )
    assert too_big.status_code == 413
    assert too_big.json()["error"]["code"] == "RequestTooLarge"


def test_the_queue_is_bounded(tmp_path: Path) -> None:
    settings = Settings(
        workspace=tmp_path, scenarios_dir=CONFIGS, max_concurrent_jobs=1, max_queued_jobs=0
    )
    with TestClient(create_app(settings)) as client:
        first = client.post(f"{API}/runs", json={"config": load_config("baseline")})
        second = client.post(f"{API}/runs", json={"config": load_config("baseline")})
        assert first.status_code == 202
        assert second.status_code == 429
        assert second.json()["error"]["code"] == "QueueFull"
        assert get(client, "/runs")["total"] == 1


def test_a_job_interrupted_by_a_restart_is_recorded_as_failed(tmp_path: Path) -> None:
    settings = Settings(workspace=tmp_path, scenarios_dir=CONFIGS)
    registry = Registry(settings.registry_path)
    registry.insert_run(
        {
            "id": "run-orphan",
            "name": "orphan",
            "seed": 1,
            "horizon_ns": SECOND,
            "status": "running",
            "created_at": now(),
            "engine_version": "0.4.0",
            "config_digest": "0" * 64,
            "config_json": load_config("baseline"),
        }
    )
    registry.close()
    with TestClient(create_app(settings)) as client:
        assert get(client, "/health")["interrupted_at_startup"] == 1
        run = get(client, "/runs/run-orphan")
        assert run["status"] == "failed"
        assert run["error"]["code"] == "WorkerInterrupted"
        # The service still works after recovering.
        fresh = submit_run(client, short(load_config("baseline"), 10))
        assert fresh["status"] == "completed"
        client.delete(f"{API}/runs/{fresh['id']}")
        assert client.get(f"{API}/runs/{fresh['id']}").status_code == 404
        assert not (settings.runs_dir / fresh["id"]).exists()
        time.sleep(0)
