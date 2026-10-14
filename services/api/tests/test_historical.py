"""Offline native historical import and mocked Alpaca acquisition."""

from __future__ import annotations

import json
import time
from pathlib import Path
from typing import Any

import httpx
import pytest
from fastapi.testclient import TestClient

from markettwin_api.app import create_app
from markettwin_api.historical_providers import AlpacaProvider
from markettwin_api.service import ServiceError
from markettwin_api.settings import Settings


def settle(client: TestClient, import_id: str) -> dict[str, Any]:
    for _ in range(100):
        row: dict[str, Any] = client.get(f"/api/v1/historical/imports/{import_id}").json()
        if row["status"] in ("completed", "failed"):
            return row
        time.sleep(0.02)
    raise AssertionError("historical import did not settle")


def test_offline_import_is_idempotent_and_replay_is_native(tmp_path: Path) -> None:
    with TestClient(create_app(Settings(workspace=tmp_path))) as client:
        body = {"provider": "local_file", "symbol": "AAPL", "feed": "fabricated_l1"}
        first = client.post("/api/v1/historical/imports", json=body)
        assert first.status_code == 202
        import_id = first.json()["id"]
        assert settle(client, import_id)["status"] == "completed"
        second = client.post("/api/v1/historical/imports", json=body)
        assert second.json()["id"] == import_id
        session = client.get(f"/api/v1/historical/sessions/{import_id}").json()
        assert session["observation_kind"] == "FABRICATED TEST DATA"
        assert session["quality"]["locked_quotes"] == 1
        assert session["quality"]["crossed_quotes"] == 1
        manifest = session["manifest"]
        assert manifest["kind"] == "observed_historical_market"
        assert manifest["record_count"] == 5
        assert session["metrics"]["vwap_micro_usd"] == 170128077
        assert (tmp_path / "historical" / "raw" / import_id / "records.jsonl").is_file()
        page = client.get(f"/api/v1/historical/sessions/{import_id}/events?offset=1&limit=2").json()
        assert [record["sequence"] for record in page["records"]] == [2, 3]
        assert page["next_offset"] == 3
        samples = client.get(f"/api/v1/historical/sessions/{import_id}/series").json()
        assert samples["records"][2]["quote_state"] == "locked"
        assert samples["records"][3]["quote_state"] == "crossed"
        trades = client.get(f"/api/v1/historical/sessions/{import_id}/trades").json()
        quotes = client.get(f"/api/v1/historical/sessions/{import_id}/quotes").json()
        assert len(trades["records"]) == 2
        assert len(quotes["records"]) == 3
        assert client.post(f"/api/v1/historical/sessions/{import_id}/verify").json()["verified"]
        assert client.get("/api/v1/ai/investigations").status_code == 200


def test_offline_import_does_not_accept_paths_or_unapproved_provider(tmp_path: Path) -> None:
    with TestClient(create_app(Settings(workspace=tmp_path))) as client:
        invalid = client.post(
            "/api/v1/historical/imports",
            json={"provider": "local_file", "symbol": "../AAPL", "feed": "fabricated_l1"},
        )
        assert invalid.status_code == 422
        denied = client.post(
            "/api/v1/historical/imports",
            json={
                "provider": "alpaca",
                "symbol": "AAPL",
                "feed": "sip",
                "start": "2024-03-11T13:30:00Z",
                "end": "2024-03-11T20:00:00Z",
            },
        )
        assert denied.status_code == 409
        assert denied.json()["error"]["code"] == "AccessConfirmationRequired"
        malformed = client.post(
            "/api/v1/historical/imports",
            json={
                "provider": "alpaca",
                "symbol": "AAPL",
                "feed": "iex",
                "start": "not-a-time",
                "end": "2024-03-11T20:00:00Z",
                "confirm_access": True,
            },
        )
        assert malformed.status_code == 422
        assert malformed.json()["error"]["code"] == "InvalidProviderRequest"
        assert client.get("/api/v1/historical/sessions/hist-unknown").status_code == 404
        assert client.get("/api/v1/historical/imports/not-an-id").status_code == 404


def test_failed_import_can_be_retried_without_losing_failed_evidence(tmp_path: Path) -> None:
    fixtures = tmp_path / "fixtures"
    settings = Settings(workspace=tmp_path / "workspace", historical_sources_dir=fixtures)
    body = {"provider": "local_file", "symbol": "AAPL", "feed": "fabricated_l1"}
    with TestClient(create_app(settings)) as client:
        first_id = client.post("/api/v1/historical/imports", json=body).json()["id"]
        first = settle(client, first_id)
        assert first["status"] == "failed"
        assert first["error_code"] == "FixtureUnavailable"

        fixtures.mkdir()
        (fixtures / "aapl_records.jsonl").write_text(
            '{"kind":"trade","symbol":"AAPL","t":"2024-03-11T13:30:00Z","p":"1.25","s":2}\n'
        )
        second_id = client.post("/api/v1/historical/imports", json=body).json()["id"]
        assert second_id != first_id
        assert settle(client, second_id)["status"] == "completed"
        assert client.get(f"/api/v1/historical/imports/{first_id}").json()["status"] == "failed"
        sessions = client.get("/api/v1/historical/sessions").json()
        assert sessions["count"] == len(sessions["sessions"]) == 1


def test_alpaca_import_flows_through_http_to_a_verified_session(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    def handler(request: httpx.Request) -> httpx.Response:
        if request.url.path.endswith("/trades"):
            return httpx.Response(
                200,
                json={
                    "trades": [
                        {
                            "t": "2024-03-11T13:30:00.000000001Z",
                            "p": 170.12,
                            "s": 10,
                            "i": 7,
                            "x": "V",
                        }
                    ],
                    "next_page_token": None,
                },
            )
        return httpx.Response(
            200,
            json={
                "quotes": [
                    {
                        "t": "2024-03-11T13:30:00.000000002Z",
                        "bp": 170.11,
                        "ap": 170.13,
                        "bs": 2,
                        "as": 3,
                        "bx": "V",
                        "ax": "V",
                    }
                ],
                "next_page_token": None,
            },
        )

    original = httpx.Client
    transport = httpx.MockTransport(handler)
    monkeypatch.setattr(httpx, "Client", lambda **kwargs: original(transport=transport, **kwargs))
    settings = Settings(
        workspace=tmp_path, alpaca_key_id="test-id", alpaca_secret_key="test-secret"
    )
    with TestClient(create_app(settings)) as client:
        response = client.post(
            "/api/v1/historical/imports",
            json={
                "provider": "alpaca",
                "symbol": "AAPL",
                "feed": "iex",
                "start": "2024-03-11T13:30:00Z",
                "end": "2024-03-11T14:00:00Z",
                "confirm_access": True,
            },
        )
        assert response.status_code == 202
        import_id = response.json()["id"]
        assert settle(client, import_id)["status"] == "completed"
        session = client.get(f"/api/v1/historical/sessions/{import_id}").json()
        assert session["observation_kind"] == "REAL HISTORICAL DATA"
        assert session["manifest"]["record_count"] == 2
        assert client.post(f"/api/v1/historical/sessions/{import_id}/verify").json()["verified"]


def test_alpaca_pagination_and_exact_decimal_conversion(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    calls: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        calls.append(request)
        if request.url.path.endswith("/trades"):
            if "page_token" not in request.url.params:
                return httpx.Response(
                    200,
                    json={
                        "trades": [
                            {
                                "t": "2024-03-11T13:30:00.000000001Z",
                                "p": 170.123456,
                                "s": 1,
                                "i": 1,
                                "x": "V",
                            }
                        ],
                        "next_page_token": "two",
                    },
                )
            return httpx.Response(200, json={"trades": [], "next_page_token": None})
        return httpx.Response(
            200,
            json={
                "quotes": [
                    {
                        "t": "2024-03-11T13:30:00.000000002Z",
                        "bp": 170.12,
                        "ap": 170.13,
                        "bs": 2,
                        "as": 3,
                    }
                ],
                "next_page_token": None,
            },
        )

    original = httpx.Client
    transport = httpx.MockTransport(handler)
    monkeypatch.setattr(httpx, "Client", lambda **kwargs: original(transport=transport, **kwargs))
    provider = AlpacaProvider(
        Settings(workspace=tmp_path, alpaca_key_id="test-id", alpaca_secret_key="test-secret")
    )
    acquired = provider.acquire(
        "AAPL", "iex", "2024-03-11T13:30:00Z", "2024-03-11T14:00:00Z", tmp_path / "raw"
    )
    lines = [json.loads(line) for line in acquired.input_path.read_text().splitlines()]
    assert lines[0]["p"] == "170.123456"
    assert lines[1]["bp"] == "170.12"
    assert len(calls) == 3
    assert calls[0].headers["APCA-API-KEY-ID"] == "test-id"
    assert calls[0].url.params["feed"] == "iex"
    assert "test-secret" not in acquired.input_path.read_text()


def test_alpaca_authentication_error_is_sanitized(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    original = httpx.Client
    transport = httpx.MockTransport(lambda _: httpx.Response(401, text="secret response"))
    monkeypatch.setattr(httpx, "Client", lambda **kwargs: original(transport=transport, **kwargs))
    provider = AlpacaProvider(
        Settings(workspace=tmp_path, alpaca_key_id="id", alpaca_secret_key="secret")
    )
    with pytest.raises(ServiceError) as error:
        provider.acquire(
            "AAPL",
            "iex",
            "2024-03-11T13:30:00Z",
            "2024-03-11T14:00:00Z",
            tmp_path / "raw",
        )
    assert error.value.code == "ProviderAuthentication"
    assert "secret" not in error.value.message


def test_alpaca_retries_rate_limits_and_reports_entitlement(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    attempts = 0

    def limited(_: httpx.Request) -> httpx.Response:
        nonlocal attempts
        attempts += 1
        return (
            httpx.Response(429)
            if attempts < 3
            else httpx.Response(200, json={"trades": [], "next_page_token": None})
        )

    monkeypatch.setattr(time, "sleep", lambda _: None)
    provider = AlpacaProvider(
        Settings(workspace=tmp_path, alpaca_key_id="id", alpaca_secret_key="secret")
    )
    response = provider._get(
        httpx.Client(transport=httpx.MockTransport(limited)),
        "https://data.alpaca.markets/v2/stocks/AAPL/trades",
        {},
    )
    assert response.status_code == 200
    assert attempts == 3

    denied = httpx.Client(transport=httpx.MockTransport(lambda _: httpx.Response(403)))
    with pytest.raises(ServiceError) as error:
        provider._get(denied, "https://data.alpaca.markets/v2/stocks/AAPL/trades", {})
    assert error.value.code == "ProviderEntitlement"
