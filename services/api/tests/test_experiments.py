"""Experiments through the API: results are the core's, cells are reproducible."""

from __future__ import annotations

import json

import markettwin
from fastapi.testclient import TestClient

from markettwin_api.settings import Settings
from tests.conftest import CONFIGS, SECOND, Json, finished, load_config, wait

API = "/api/v1"


def smoke(name: str = "maker_smoke", seeds: object = None, horizon: int = 120) -> Json:
    experiment: Json = json.loads(
        (CONFIGS / "experiments" / "maker_comparison_smoke.json").read_text()
    )
    experiment.update(
        experiment_id=name,
        base_config=load_config("baseline"),
        horizon_ns=horizon * SECOND,
        artifacts="None",
        limits={"max_runs": 400, "max_output_bytes": 0},
    )
    if seeds is not None:
        experiment["seeds"] = seeds
    return experiment


def submit(client: TestClient, experiment: Json) -> Json:
    response = client.post(
        f"{API}/experiments", json={"config": experiment, "scenario_key": "baseline"}
    )
    assert response.status_code == 202, response.text
    return wait(client, f"{API}/experiments/{response.json()['id']}", finished)


def test_validation_reports_the_design_and_refuses_oversized_or_path_based_configs(
    client: TestClient,
) -> None:
    def validate(config: object) -> Json:
        body: Json = client.post(f"{API}/experiments/validate", json={"config": config}).json()
        return body

    ok = validate(smoke())
    assert ok["valid"] is True
    assert ok["design"]["seeds"] == 4 and ok["design"]["total_runs"] == 8
    assert ok["design"]["per_run"]["horizon_ns"] == 120 * SECOND
    assert ok["canonical_config"]["seeds"] == [1, 2, 3, 4]

    # The CLI resolves a relative path here; a request may not name a file.
    by_path = smoke() | {"base_config": "../baseline.json"}
    assert validate(by_path)["errors"][0]["path"] == "base_config"
    assert validate(by_path)["errors"][0]["source"] == "schema"

    huge = smoke(seeds={"first": 1, "count": 5000}) | {
        "limits": {"max_runs": 10000, "max_output_bytes": 0}
    }
    (limit,) = validate(huge)["errors"]
    assert (limit["source"], limit["path"]) == ("api", "seeds")

    undersized = smoke() | {"limits": {"max_runs": 4, "max_output_bytes": 0}}
    (engine,) = validate(undersized)["errors"]
    assert engine["source"] == "engine"
    assert "exceeds limits.max_runs" in engine["message"]

    unknown = smoke()
    unknown["comparison"]["candidate"] = "nobody"
    assert validate(unknown)["errors"][0]["source"] == "engine"
    assert client.post(f"{API}/experiments", json={"config": unknown}).status_code == 422


def test_results_are_the_native_document_and_every_interval_is_the_cores(
    client: TestClient, settings: Settings
) -> None:
    experiment = submit(client, smoke())
    assert experiment["status"] == "completed"
    assert (experiment["total_runs"], experiment["completed_runs"], experiment["failed_runs"]) == (
        8,
        8,
        0,
    )
    assert experiment["pairs_completed"] == 4
    assert experiment["variants"] == ["baseline_maker", "inventory_maker"]
    assert experiment["progress"] is None

    document = client.get(f"{API}/experiments/{experiment['id']}/results").json()["document"]
    stored = json.loads(
        (settings.experiments_dir / experiment["id"] / "native" / "experiment.json").read_text()
    )
    assert document == stored
    assert document["results_digest"] == experiment["results_digest"]

    # Run the same experiment directly against the core: every statistic must be identical.
    direct = markettwin.run_experiment(smoke())
    assert document["results"] == direct.results
    metric = document["results"]["comparison"]["metrics"][0]
    assert metric["name"] == "total_pnl"
    assert experiment["headline"]["difference"] == metric["difference"]
    assert metric["difference"]["mean_t_interval_milli"] is not None
    assert metric["difference"]["mean_bootstrap_interval_milli"] is not None

    values = client.get(f"{API}/experiments/{experiment['id']}/paired-values").json()["document"]
    assert values == direct.paired_values
    assert values["seeds"] == [1, 2, 3, 4]

    listed = client.get(f"{API}/experiments").json()
    assert experiment["id"] in [item["id"] for item in listed["experiments"]]


def test_a_cell_is_materialized_as_a_run_whose_digests_match_the_experiment(
    client: TestClient,
) -> None:
    experiment = submit(client, smoke("cells"))
    path = f"{API}/experiments/{experiment['id']}/cells/materialize"
    response = client.post(path, json={"variant": "inventory_maker", "seed": 2})
    assert response.status_code == 202, response.text
    run = wait(client, f"{API}/runs/{response.json()['id']}", finished)
    assert run["status"] == "completed"
    assert run["name"] == "cells-inventory_maker-s2"
    assert (run["source_kind"], run["experiment_id"], run["variant"]) == (
        "experiment_cell",
        experiment["id"],
        "inventory_maker",
    )
    assert run["cell"]["digests_match"] is True and run["cell"]["mismatched"] == []
    document = client.get(f"{API}/experiments/{experiment['id']}/results").json()["document"]
    recorded = next(
        item
        for item in document["results"]["runs"]
        if item["variant"] == "inventory_maker" and item["seed"] == 2
    )
    assert run["summary"]["trace_digest"] == recorded["trace_digest"]
    assert run["manifest"]["artifacts"]["accounting.jsonl"] == recorded["accounting_digest"]
    # The subject's policy is the candidate's in this cell.
    subject = next(agent for agent in run["agents"] if agent["id"] == 3)
    assert subject["policy"] == "InventoryMarketMaker"

    again = client.post(path, json={"variant": "inventory_maker", "seed": 2})
    assert again.json()["id"] == run["id"]
    detail = client.get(f"{API}/experiments/{experiment['id']}").json()
    assert detail["cell_runs"] == [
        {
            "run_id": run["id"],
            "variant": "inventory_maker",
            "seed": 2,
            "status": "completed",
            "digests_match": True,
        }
    ]
    assert client.post(path, json={"variant": "nobody", "seed": 2}).status_code == 404
    assert client.post(path, json={"variant": "inventory_maker", "seed": 99}).status_code == 404


def test_a_running_experiment_reports_counts_and_can_be_canceled(client: TestClient) -> None:
    long = smoke("canceled", seeds={"first": 1, "count": 150}, horizon=3600)
    response = client.post(f"{API}/experiments", json={"config": long})
    assert response.status_code == 202
    experiment = response.json()["id"]
    running = wait(
        client,
        f"{API}/experiments/{experiment}",
        lambda body: body["progress"] is not None and body["progress"]["completed"] >= 2,
    )
    assert running["status"] == "running"
    assert running["progress"]["total"] == 300
    assert running["completed_runs"] == running["progress"]["completed"]
    assert running["progress"]["current_run_id"].startswith("canceled-")
    assert client.get(f"{API}/experiments/{experiment}/results").status_code == 409

    assert client.post(f"{API}/experiments/{experiment}/cancel").status_code == 202
    done = wait(client, f"{API}/experiments/{experiment}", finished)
    assert done["status"] == "canceled"
    assert 2 <= done["completed_runs"] < 300
    assert done["completed_runs"] + done["failed_runs"] == 300
    results = client.get(f"{API}/experiments/{experiment}/results").json()["document"]["results"]
    codes = {run["error_code"] for run in results["runs"] if run["status"] == "Failed"}
    assert codes == {"Canceled"}
    assert results["comparison"]["pairs_completed"] == done["pairs_completed"]
    assert client.post(f"{API}/experiments/{experiment}/cancel").status_code == 409
    assert client.get(f"{API}/experiments/missing").status_code == 404
