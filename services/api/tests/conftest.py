"""Fixtures run the real engine: nothing in these tests is mocked."""

from __future__ import annotations

import json
import time
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest
from fastapi.testclient import TestClient

from markettwin_api.app import create_app
from markettwin_api.settings import Settings

REPOSITORY = Path(__file__).resolve().parents[3]
CONFIGS = REPOSITORY / "configs"
SECOND = 1_000_000_000

Json = dict[str, Any]


def load_config(name: str) -> Json:
    config: Json = json.loads((CONFIGS / f"{name}.json").read_text())
    return config


def short(config: Json, seconds: int, name: str | None = None) -> Json:
    return {**config, "horizon_ns": seconds * SECOND, "run_id": name or config["run_id"]}


def wait(client: TestClient, path: str, done: Any, timeout: float = 120.0) -> Json:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        body: Json = client.get(path).json()
        if done(body):
            return body
        time.sleep(0.05)
    raise AssertionError(f"{path} did not settle within {timeout} s: {body}")


def finished(body: Json) -> bool:
    return body["status"] not in ("pending", "running")


def submit_run(client: TestClient, config: Json, scenario_key: str | None = None) -> Json:
    response = client.post("/api/v1/runs", json={"config": config, "scenario_key": scenario_key})
    assert response.status_code == 202, response.text
    return wait(client, f"/api/v1/runs/{response.json()['id']}", finished)


@pytest.fixture(scope="session")
def settings(tmp_path_factory: pytest.TempPathFactory) -> Settings:
    return Settings(workspace=tmp_path_factory.mktemp("workspace"), scenarios_dir=CONFIGS)


@pytest.fixture(scope="session")
def client(settings: Settings) -> Iterator[TestClient]:
    with TestClient(create_app(settings)) as test_client:
        yield test_client


@pytest.fixture(scope="session")
def baseline(client: TestClient) -> Json:
    """The shipped one-hour baseline, run once through the API."""
    return submit_run(client, load_config("baseline"), "baseline")


@pytest.fixture(scope="session")
def liquidity(client: TestClient) -> Json:
    return submit_run(client, load_config("liquidity_shock"), "liquidity_shock")
