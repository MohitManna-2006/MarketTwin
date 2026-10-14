"""The live hub end to end: mock provider, native normalizer, HTTP, WebSocket, recording.

Everything runs offline against a loopback implementation of the provider protocol.
"""

from __future__ import annotations

import asyncio
import json
from collections.abc import Iterator
from datetime import datetime
from decimal import Decimal
from pathlib import Path
from typing import Any
from zoneinfo import ZoneInfo

import pytest
from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

from markettwin_api.app import create_app
from markettwin_api.historical_service import HistoricalResearch
from markettwin_api.live_gateway import Frame, render_prices, resolve_endpoint
from markettwin_api.live_recording import CaptureWriter
from markettwin_api.live_service import LiveService, session_hint
from markettwin_api.service import Research
from markettwin_api.settings import Settings

from .live_support import ThreadedMock, eventually, quote, trade, until

Json = dict[str, Any]
T0 = "2024-03-11T13:30:00.000000001Z"
T1 = "2024-03-11T13:30:00.000000002Z"
T2 = "2024-03-11T13:30:01.5Z"


@pytest.fixture
def mock() -> Iterator[ThreadedMock]:
    server = ThreadedMock()
    yield server
    server.close()


def live_settings(tmp_path: Path, mock: ThreadedMock, **overrides: Any) -> Settings:
    values: dict[str, Any] = {
        "workspace": tmp_path / "workspace",
        "live_feed": "mock",
        "live_mock_url": mock.url,
        "live_backoff_base_seconds": 0.02,
        "live_backoff_max_seconds": 0.1,
        "live_flush_interval_seconds": 0.002,
        "live_heartbeat_seconds": 0.05,
        "live_handshake_timeout_seconds": 1.0,
    }
    return Settings(**{**values, **overrides})


def feed_state(client: TestClient) -> str:
    state: str = client.get("/api/v1/live/status").json()["feed"]["state"]
    return state


def start(client: TestClient, feed: str | None = None) -> None:
    response = client.post("/api/v1/live/feed/start", json={"feed": feed})
    assert response.status_code == 200, response.text
    until(lambda: feed_state(client) == "CONNECTED", message="feed CONNECTED")


def receive(socket: Any, kind: str, limit: int = 200) -> Json:
    for _ in range(limit):
        message: Json = socket.receive_json()
        if message["type"] == kind:
            return message
    raise AssertionError(f"no {kind} message arrived")


def test_a_stopped_feed_is_explicit_and_leaves_the_rest_of_the_api_working(
    tmp_path: Path, mock: ThreadedMock
) -> None:
    settings = Settings(workspace=tmp_path / "workspace")
    with TestClient(create_app(settings)) as client:
        status = client.get("/api/v1/live/status").json()
        assert status["feed"]["state"] == "STOPPED"
        assert status["feed"]["feed"] == "iex"
        assert status["feed"]["credentials_configured"] is False
        assert status["market"]["activity"] == "offline"
        assert client.get("/api/v1/live/health").json()["status"] == "stopped"
        assert client.get("/api/v1/live/symbols").json()["symbols"] == [
            "AAPL",
            "NVDA",
            "MSFT",
            "SPY",
        ]
        refused = client.post("/api/v1/live/feed/start", json={"feed": "iex"})
        assert refused.status_code == 503
        assert refused.json()["error"]["code"] == "ProviderNotConfigured"
        assert feed_state(client) == "STOPPED"
        assert client.post("/api/v1/live/recordings", json={}).status_code == 409
        assert client.get("/api/v1/health").json()["schemas"]["live"] == 1
        assert client.get("/api/v1/scenarios").status_code == 200
    assert mock.stream.accepted == 0


def test_autostart_failure_is_isolated_and_reported(tmp_path: Path) -> None:
    settings = Settings(workspace=tmp_path / "workspace", live_feed="iex", live_autostart=True)
    with TestClient(create_app(settings)) as client:
        health = client.get("/api/v1/live/health").json()
        assert health["status"] == "failed"
        assert health["last_error"]["code"] == "ProviderNotConfigured"
        assert client.get("/api/v1/health").status_code == 200


def test_stream_delivers_a_snapshot_then_ordered_deltas_with_provenance(
    tmp_path: Path, mock: ThreadedMock
) -> None:
    with TestClient(create_app(live_settings(tmp_path, mock))) as client:
        start(client)
        with client.websocket_connect("/api/v1/live/stream") as socket:
            hello = socket.receive_json()
            assert hello["type"] == "hello" and hello["v"] == 1
            assert hello["limits"]["client_queue_messages"] == 256
            snapshot = socket.receive_json()
            assert snapshot["type"] == "snapshot"
            assert snapshot["status"]["feed"]["state"] == "CONNECTED"
            assert snapshot["status"]["feed"]["source_label"] == "FABRICATED MOCK DATA"
            assert snapshot["status"]["feed"]["test_data"] is True
            # Connected is not the same as updating: nothing has been observed yet.
            assert snapshot["status"]["market"]["activity"] == "none_yet"
            assert snapshot["states"] == {} and snapshot["recent"] == []

            mock.emit([quote("FAKEPACA", 170.09, 170.11, T0), trade("FAKEPACA", 170.10, 5, 1, T1)])
            events = receive(socket, "events")
            assert events["stream"] == snapshot["stream"]
            assert events["seq"] > snapshot["seq"]
            kinds = [event["kind"] for event in events["events"]]
            assert kinds == ["quote", "trade"]
            trade_event = events["events"][1]
            assert trade_event["price_micro_usd"] == 170_100_000
            assert trade_event["source_price"] == "170.1"
            assert trade_event["timestamp_ns"] == "1710163800000000002"
            assert trade_event["timestamp_precision_digits"] == 9
            assert trade_event["feed"] == "mock" and trade_event["provider"] == "mock_live"
            assert int(trade_event["receipt_wall_ns"]) > 0
            state = events["states"]["FAKEPACA"]
            assert state["quote_state"] == "two_sided"
            assert state["spread_micro_usd"] == 20_000
            assert state["midpoint_micro_usd"] == 170_100_000
            assert state["last_trade"]["quantity"] == 5
            assert state["quote"]["bid_micro_usd"] == 170_090_000

            status = receive(socket, "status")
            assert status["status"]["market"]["activity"] == "active"
            assert status["states"]["FAKEPACA"]["freshness"]["status"] == "fresh"

            socket.send_json({"type": "resync"})
            again = receive(socket, "snapshot")
            assert [event["kind"] for event in again["recent"]] == ["quote", "trade"]
            assert again["states"]["FAKEPACA"]["last_trade"]["price_micro_usd"] == 170_100_000
        document = client.get("/api/v1/live/snapshot").json()
        assert document["type"] == "snapshot" and len(document["recent"]) == 2
        pipeline = client.get("/api/v1/live/status").json()["pipeline"]
        assert pipeline["events"] == 2 and pipeline["dropped_messages"] == 0
        assert pipeline["local_latency_us"]["samples"] >= 1


def test_many_readers_share_one_upstream_connection(tmp_path: Path, mock: ThreadedMock) -> None:
    with TestClient(create_app(live_settings(tmp_path, mock))) as client:
        start(client)
        with (
            client.websocket_connect("/api/v1/live/stream") as first,
            client.websocket_connect("/api/v1/live/stream") as second,
            client.websocket_connect("/api/v1/live/stream") as third,
        ):
            for socket in (first, second, third):
                receive(socket, "snapshot")
            mock.emit([trade("FAKEPACA", 10.01, 1, 1, T0)])
            received = [receive(socket, "events") for socket in (first, second, third)]
            assert len({message["seq"] for message in received}) == 1
            assert all(message["events"][0]["trade_id"] == 1 for message in received)
            assert client.get("/api/v1/live/status").json()["clients"]["connected"] == 3
    assert mock.stream.accepted == 1
    assert mock.stream.auth_attempts == 1


def test_reader_capacity_is_bounded(tmp_path: Path, mock: ThreadedMock) -> None:
    with (
        TestClient(create_app(live_settings(tmp_path, mock, live_max_clients=1))) as client,
        client.websocket_connect("/api/v1/live/stream") as first,
    ):
        receive(first, "snapshot")
        with client.websocket_connect("/api/v1/live/stream") as second:
            with pytest.raises(WebSocketDisconnect) as closed:
                second.receive_json()
            assert closed.value.code == 1013
        assert client.get("/api/v1/live/status").json()["clients"]["refused"] == 1


def test_stale_quotes_and_quiet_markets_are_reported_while_connected(
    tmp_path: Path, mock: ThreadedMock
) -> None:
    settings = live_settings(tmp_path, mock, live_stale_after_seconds=0.2)
    with TestClient(create_app(settings)) as client:
        start(client)
        mock.emit([quote("FAKEPACA", 10.00, 10.02, T0)])
        until(lambda: client.get("/api/v1/live/status").json()["market"]["activity"] == "active")
        quiet = until(
            lambda: (
                (
                    (status := client.get("/api/v1/live/status").json())["market"]["activity"]
                    == "quiet"
                )
                and status
            )
        )
        # The transport is healthy the whole time; only the observations have gone quiet.
        assert quiet["feed"]["state"] == "CONNECTED"
        assert quiet["market"]["session"] == "not_applicable"
        state = client.get("/api/v1/live/snapshot").json()["states"]["FAKEPACA"]
        assert state["freshness"]["status"] == "stale"
        assert state["freshness"]["quote_stale"] is True
        assert state["freshness"]["spread_quality"] == "stale_quote"
        assert state["spread_micro_usd"] == 20_000
        assert state["last_trade"] is None


def test_upstream_loss_is_visible_then_recovers_with_its_subscription(
    tmp_path: Path, mock: ThreadedMock
) -> None:
    with TestClient(create_app(live_settings(tmp_path, mock))) as client:
        start(client)
        with client.websocket_connect("/api/v1/live/stream") as socket:
            receive(socket, "snapshot")
            mock.drop(refuse_seconds=0.3)
            down = None
            for _ in range(400):
                message = socket.receive_json()
                if message["type"] == "status" and message["status"]["feed"]["state"] in (
                    "RECONNECTING",
                    "CONNECTING",
                ):
                    down = message
                    break
            assert down is not None
            assert down["status"]["market"]["activity"] == "offline"
            until(lambda: feed_state(client) == "CONNECTED")
            mock.emit([trade("FAKEPACA", 10.05, 2, 7, T2)])
            assert receive(socket, "events")["events"][0]["trade_id"] == 7
        status = client.get("/api/v1/live/status").json()
        assert status["feed"]["reconnects"] >= 1
        assert status["feed"]["subscribed"]["trades"] == ["FAKEPACA"]
        assert client.get("/api/v1/live/health").json()["status"] == "ok"
        assert any(item["to_state"] == "RECONNECTING" for item in status["feed"]["transitions"])


def test_controls_are_local_or_token_only_and_origins_are_checked(
    tmp_path: Path, mock: ThreadedMock
) -> None:
    settings = live_settings(tmp_path, mock, live_control_token="s3cret-token")
    token = {"x-markettwin-control-token": "s3cret-token"}
    with TestClient(create_app(settings), client=("203.0.113.9", 4000)) as remote:
        assert remote.get("/api/v1/live/status").status_code == 200
        for path in ("/feed/start", "/feed/stop", "/recordings", "/recordings/stop"):
            denied = remote.post(f"/api/v1/live{path}", json={})
            assert denied.status_code == 403, path
            assert denied.json()["error"]["code"] == "ControlNotPermitted"
        assert remote.post("/api/v1/live/symbols", json={"symbols": ["AAPL"]}).status_code == 403
        wrong = remote.post(
            "/api/v1/live/feed/start", json={}, headers={"x-markettwin-control-token": "guess"}
        )
        assert wrong.status_code == 403
        assert mock.stream.accepted == 0
        assert remote.post("/api/v1/live/feed/start", json={}, headers=token).status_code == 200
        # A page from another site is refused even when it presents the token.
        foreign = remote.post(
            "/api/v1/live/feed/stop", json={}, headers={**token, "origin": "https://evil.example"}
        )
        assert foreign.status_code == 403
        assert foreign.json()["error"]["code"] == "OriginNotAllowed"
        with (
            pytest.raises(WebSocketDisconnect),
            remote.websocket_connect(
                "/api/v1/live/stream", headers={"origin": "https://evil.example"}
            ),
        ):
            pass
        with remote.websocket_connect(
            "/api/v1/live/stream", headers={"origin": "http://127.0.0.1:5173"}
        ) as socket:
            assert socket.receive_json()["type"] == "hello"
        assert "s3cret-token" not in remote.get("/api/v1/live/status").text
        assert remote.post("/api/v1/live/feed/stop", json={}, headers=token).status_code == 200


def test_watchlist_validation_and_live_subscription_changes(
    tmp_path: Path, mock: ThreadedMock
) -> None:
    settings = live_settings(tmp_path, mock, live_max_symbols=3)
    with TestClient(create_app(settings)) as client:
        post = lambda body: client.post("/api/v1/live/symbols", json=body)  # noqa: E731
        assert post({"symbols": ["aapl$"]}).json()["error"]["code"] == "InvalidSymbol"
        assert (
            post({"symbols": ["A", "B", "C", "D"]}).json()["error"]["code"] == "SymbolLimitExceeded"
        )
        assert post({"symbols": ["AAPL"], "feed": "test"}).json()["error"]["code"] == (
            "TestFeedSymbol"
        )
        assert post({"symbols": ["FAKEPACA"], "feed": "test"}).json()["editable"] is False
        assert post({"symbols": ["aapl", "AAPL", "MSFT"], "feed": "mock"}).json()["symbols"] == [
            "AAPL",
            "MSFT",
        ]
        start(client)
        assert client.get("/api/v1/live/symbols").json()["subscribed"]["trades"] == [
            "AAPL",
            "MSFT",
        ]
        mock.emit([trade("AAPL", 1.5, 1, 1, T0), trade("MSFT", 2.5, 1, 2, T0)])
        until(lambda: len(client.get("/api/v1/live/snapshot").json()["states"]) == 2)
        assert post({"symbols": ["MSFT", "SPY"]}).status_code == 200
        until(
            lambda: (
                client.get("/api/v1/live/symbols").json()["subscribed"]["quotes"] == ["MSFT", "SPY"]
            )
        )
        assert list(client.get("/api/v1/live/snapshot").json()["states"]) == ["MSFT"]
        assert post({"symbols": ["MSFT"], "feed": "iex"}).json()["error"]["code"] == (
            "FeedAlreadyRunning"
        )


def test_invalid_records_are_rejected_natively_with_reasons(
    tmp_path: Path, mock: ThreadedMock
) -> None:
    with TestClient(create_app(live_settings(tmp_path, mock))) as client:
        start(client)
        bad_size = trade("FAKEPACA", 10.0, 1, 2, T0)
        bad_size["s"] = 0.5
        correction = {"T": "c", "S": "FAKEPACA", "x": "V", "oi": 1, "op": 10.0, "cp": 10.1}
        mock.emit(
            [
                trade("FAKEPACA", 10.1234567, 1, 1, T0),
                bad_size,
                correction,
                trade("FAKEPACA", 10.01, 3, 3, T1),
            ]
        )
        pipeline = until(
            lambda: (
                (status := client.get("/api/v1/live/status").json()["pipeline"])["events"] == 1
                and status
            )
        )
        assert pipeline["rejected"] == 2
        assert pipeline["unsupported"] == {"c": 1}
        reasons = {item["reason"] for item in pipeline["recent_rejects"]}
        assert reasons == {"price exceeds six decimal places: p", "size must be an integer: s"}


def record_session(client: TestClient, mock: ThreadedMock) -> Json:
    started = client.post("/api/v1/live/recordings", json={})
    assert started.status_code == 201, started.text
    recording_id = started.json()["id"]
    assert client.post("/api/v1/live/recordings", json={}).json()["error"]["code"] == (
        "RecordingActive"
    )
    mock.emit([quote("FAKEPACA", 170.09, 170.11, T0), trade("FAKEPACA", 170.10, 5, 1, T1)])
    until(lambda: client.get("/api/v1/live/status").json()["recording"]["progress"]["frames"] == 1)
    mock.drop(refuse_seconds=0.2)
    until(lambda: client.get("/api/v1/live/status").json()["recording"]["gaps"] == 1)
    until(lambda: client.get("/api/v1/live/status").json()["recording"]["upstream"] == "up")
    mock.emit(
        [
            trade("FAKEPACA", 170.12, 7, 2, T2),
            trade("FAKEPACA", 170.10, 5, 1, T1),
            trade("FAKEPACA", 1.0000001, 1, 3, T2),
        ]
    )
    until(lambda: client.get("/api/v1/live/status").json()["recording"]["progress"]["frames"] == 2)
    assert client.post("/api/v1/live/recordings/stop", json={}).status_code == 202
    document: Json = until(
        lambda: (
            (row := client.get(f"/api/v1/live/recordings/{recording_id}").json())["status"]
            in ("completed", "failed")
            and row
        )
    )
    return document


def test_a_recording_becomes_a_verified_historical_session_with_its_gap(
    tmp_path: Path, mock: ThreadedMock
) -> None:
    settings = live_settings(tmp_path, mock)
    with TestClient(create_app(settings)) as client:
        start(client)
        recording = record_session(client, mock)
        assert recording["status"] == "completed", recording
        summary = recording["summary"]
        assert summary["stop_reason"] == "user_stopped"
        assert summary["source_label"] == "FABRICATED MOCK DATA"
        assert summary["frames"] == 2
        assert summary["accepted_records"] == 4
        assert summary["rejected_records"] == 1
        assert summary["reject_reasons"] == {"price exceeds six decimal places: p": 1}
        assert summary["uninterrupted"] is False
        assert len(summary["coverage_gaps"]) == 1
        gap = summary["coverage_gaps"][0]
        assert gap["closed_by"] == "upstream_restored" and gap["duration_ms"] >= 0
        assert "not a complete record" in summary["disclaimer"]

        session_id = summary["symbols"]["FAKEPACA"]["session_id"]
        session = client.get(f"/api/v1/historical/sessions/{session_id}").json()
        assert session["provider"] == "mock_live" and session["feed"] == "mock"
        assert session["observation_kind"] == "FABRICATED TEST DATA"
        assert session["quality"]["records_imported"] == 3
        assert session["quality"]["duplicates"] == 1
        capture = session["manifest"]["metadata"]["capture"]
        assert capture["recording_id"] == recording["id"]
        assert capture["coverage_gaps"] == summary["coverage_gaps"]
        assert capture["capture_sha256"] == summary["capture_sha256"]
        assert session["metrics"]["last_trade_micro_usd"] == 170_120_000
        events = client.get(f"/api/v1/historical/sessions/{session_id}/events").json()["records"]
        assert [event["source_timestamp"] for event in events] == [T0, T1, T2]
        assert events[1]["source_price"] == "170.1"
        assert client.post(f"/api/v1/historical/sessions/{session_id}/verify").json()["verified"]
        listed = client.get("/api/v1/live/recordings").json()
        assert listed["count"] == 1 and listed["recordings"][0]["id"] == recording["id"]

    directory = settings.live_dir / "recordings" / recording["id"]
    lines = [json.loads(line) for line in (directory / "capture.jsonl").read_text().splitlines()]
    assert lines[0]["type"] == "header"
    assert lines[-1]["type"] == "footer"
    frames = [line for line in lines if line["type"] == "frame"]
    # The provider payload is kept verbatim, in arrival order, with its receipt time.
    assert json.loads(frames[0]["raw"])[1]["p"] == 170.1
    assert [line["o"] for line in frames] == [1, 2]
    assert int(frames[0]["receipt_wall_ns"]) <= int(frames[1]["receipt_wall_ns"])
    events_between = [line.get("event") for line in lines[lines.index(frames[0]) : -1]]
    assert events_between.index("upstream_lost") < events_between.index("upstream_restored")
    assert "mock-secret" not in (directory / "capture.jsonl").read_text()
    assert client.get("/api/v1/live/recordings/not-an-id").status_code == 404


def test_shutdown_closes_the_feed_and_finalizes_an_open_recording(
    tmp_path: Path, mock: ThreadedMock
) -> None:
    settings = live_settings(tmp_path, mock)
    with TestClient(create_app(settings)) as client:
        start(client)
        recording_id = client.post("/api/v1/live/recordings", json={}).json()["id"]
        mock.emit([trade("FAKEPACA", 10.01, 1, 1, T0)])
        until(
            lambda: client.get("/api/v1/live/status").json()["recording"]["progress"]["frames"] == 1
        )
    until(lambda: len(mock.stream.sessions) == 0, message="upstream closed")
    with TestClient(create_app(settings)) as client:
        row = client.get(f"/api/v1/live/recordings/{recording_id}").json()
        assert row["status"] == "completed"
        assert row["summary"]["stop_reason"] == "service_shutdown"
        assert row["summary"]["symbols"]["FAKEPACA"]["status"] == "completed"
        # The endpoint lock was released: the next process can own the feed.
        start(client)


def test_an_interrupted_capture_is_recovered_with_its_interruption_recorded(
    tmp_path: Path, mock: ThreadedMock
) -> None:
    settings = live_settings(tmp_path, mock)
    endpoint = resolve_endpoint("mock", mock.url)
    recording_id = "rec-0123456789"
    with TestClient(create_app(settings)):
        pass
    research = Research(settings)
    research.registry.insert_live_recording(
        {
            "id": recording_id,
            "provider": endpoint.provider,
            "feed": endpoint.feed,
            "status": "recording",
            "created_at": "2024-03-11T13:30:00.000Z",
        }
    )
    research.close()
    writer = CaptureWriter(
        settings.live_dir / "recordings" / recording_id,
        {
            "recording_id": recording_id,
            "provider": endpoint.provider,
            "feed": endpoint.feed,
            "source_label": endpoint.source_label,
            "test_data": True,
            "scope": endpoint.scope,
            "license": endpoint.license,
            "symbols": ["FAKEPACA"],
            "started_wall_ns": "1710163800000000000",
            "started_at": "2024-03-11T13:30:00.000Z",
        },
        1 << 20,
        1000,
        3600.0,
    )
    raw = json.dumps([trade("FAKEPACA", 12.34, 3, 1, T0)])
    writer.frame(Frame(1, raw, json.loads(raw), 1710163800000000001, 5))
    writer.sync()
    # The process dies here: no footer, and the line being written is cut short.
    with writer.path.open("ab") as capture:
        capture.write(b'{"type":"frame","o":2,"raw":"[{\\"T\\":\\"t\\"')

    with TestClient(create_app(settings)) as client:
        row = until(
            lambda: (
                (item := client.get(f"/api/v1/live/recordings/{recording_id}").json())["status"]
                == "completed"
                and item
            )
        )
        summary = row["summary"]
        assert summary["stop_reason"] == "process_interrupted"
        assert summary["truncated_tail"] is True
        assert summary["accepted_records"] == 1
        assert summary["uninterrupted"] is False
        assert summary["coverage_gaps"][-1]["closed_by"] == "process_interrupted"
        assert summary["coverage_gaps"][-1]["to"] is None
        session_id = summary["symbols"]["FAKEPACA"]["session_id"]
        assert client.post(f"/api/v1/historical/sessions/{session_id}/verify").json()["verified"]
        capture_text = (
            settings.live_dir / "recordings" / recording_id / "capture.jsonl"
        ).read_text()
        assert capture_text.endswith('[{\\"T\\":\\"t\\"')  # the evidence is not rewritten


def test_a_recording_stops_itself_at_its_storage_bound(tmp_path: Path, mock: ThreadedMock) -> None:
    settings = live_settings(tmp_path, mock, live_recording_max_bytes=900)
    with TestClient(create_app(settings)) as client:
        start(client)
        recording_id = client.post("/api/v1/live/recordings", json={}).json()["id"]
        for number in range(1, 6):
            mock.emit([trade("FAKEPACA", 10.0 + number / 100, 1, number)])
        row = until(
            lambda: (
                (item := client.get(f"/api/v1/live/recordings/{recording_id}").json())["status"]
                == "completed"
                and item
            )
        )
        assert row["summary"]["stop_reason"] == "storage_limit"
        assert row["summary"]["bytes"] < 2_000
        assert 1 <= row["summary"]["frames"] < 5
        assert client.get("/api/v1/live/status").json()["recording"]["active"] is False


class FakeSocket:
    """A reader whose socket can be made to stop accepting data."""

    def __init__(self) -> None:
        self.sent: list[Json] = []
        self.gate = asyncio.Event()
        self.gate.set()
        self.closed: tuple[int, str | None] | None = None
        self._never = asyncio.Event()

    async def send_text(self, data: str) -> None:
        await self.gate.wait()
        self.sent.append(json.loads(data))

    async def receive_text(self) -> str:
        await self._never.wait()
        return ""

    async def close(self, code: int = 1000, reason: str | None = None) -> None:
        self.closed = (code, reason)


def frame(number: int, messages: list[Json]) -> Frame:
    text = json.dumps(messages)
    parsed = json.loads(text, parse_float=Decimal)
    render_prices(parsed)
    return Frame(number, text, parsed, 1_710_163_800_000_000_000 + number, number)


def hub(tmp_path: Path, mock: ThreadedMock, **overrides: Any) -> tuple[LiveService, Research]:
    research = Research(live_settings(tmp_path, mock, **overrides))
    return LiveService(research, HistoricalResearch(research)), research


def test_a_slow_reader_is_bounded_told_of_its_gap_and_resynchronized(
    tmp_path: Path, mock: ThreadedMock
) -> None:
    async def scenario() -> None:
        live, research = hub(tmp_path, mock, live_client_queue_messages=4)
        await live.startup()
        await live.start_feed(None)
        await eventually(lambda: live.status()["feed"]["state"] == "CONNECTED")
        slow, fast = FakeSocket(), FakeSocket()
        tasks = [asyncio.create_task(live.serve(socket)) for socket in (slow, fast)]
        await eventually(lambda: len(slow.sent) >= 2 and len(fast.sent) >= 2)
        slow.gate.clear()
        for number in range(1, 41):
            live._process([frame(number, [trade("FAKEPACA", 10.0, 1, number)])], 1)
            await asyncio.sleep(0)
            assert all(len(client.queue) <= 4 for client in live._clients)
        clients = live.status()["clients"]
        assert clients["overflows"] >= 1
        assert clients["dropped_events"] >= 30
        assert clients["queue_high_water"] <= 4
        # The fast reader lost nothing and saw every sequence number in order.
        await eventually(lambda: sum(m["type"] == "events" for m in fast.sent) == 40)
        fast_seqs = [m["seq"] for m in fast.sent if m["type"] == "events"]
        assert fast_seqs == sorted(fast_seqs) and len(set(fast_seqs)) == 40
        assert not any(m["type"] == "gap" for m in fast.sent)

        before = len(slow.sent)
        slow.gate.set()
        await eventually(lambda: any(m["type"] == "gap" for m in slow.sent))
        await eventually(lambda: slow.sent[-1]["type"] == "snapshot" and len(slow.sent) > before)
        gap = next(m for m in slow.sent if m["type"] == "gap")
        assert gap["reason"] == "slow_client" and gap["scope"] == "this_client"
        assert gap["dropped_events"] >= 30
        resync = slow.sent[-1]
        assert resync["states"]["FAKEPACA"]["last_trade"]["trade_id"] == 40
        assert resync["seq"] == fast_seqs[-1] or resync["seq"] > fast_seqs[-1]
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        await live.close()
        research.close()

    asyncio.run(scenario())


def test_a_reader_that_never_drains_is_disconnected(tmp_path: Path, mock: ThreadedMock) -> None:
    async def scenario() -> None:
        live, research = hub(tmp_path, mock, live_send_timeout_seconds=0.05)
        await live.startup()
        socket = FakeSocket()
        socket.gate.clear()
        await asyncio.wait_for(live.serve(socket), 5)
        assert socket.closed is not None and socket.closed[0] == 1013
        assert live.status()["clients"]["slow_disconnects"] == 1
        assert live.status()["clients"]["connected"] == 0
        await live.close()
        research.close()

    asyncio.run(scenario())


def test_the_inbox_is_bounded_and_its_overflow_is_announced(
    tmp_path: Path, mock: ThreadedMock
) -> None:
    async def scenario() -> None:
        live, research = hub(tmp_path, mock, live_inbox_frames=8)
        await live.start_feed(None)  # no pump task: the inbox can only fill
        socket = FakeSocket()
        reader = asyncio.create_task(live.serve(socket))
        await eventually(lambda: len(socket.sent) >= 2)
        for number in range(1, 21):
            live._on_frame(frame(number, [trade("FAKEPACA", 10.0, 1, number)]))
        assert len(live._inbox) == 8
        pipeline = live.status()["pipeline"]
        assert pipeline["dropped_frames"] == 12 and pipeline["dropped_messages"] == 12
        assert pipeline["inbox_high_water"] == 8
        live._process(list(live._inbox), 8)
        await eventually(lambda: any(m["type"] == "gap" for m in socket.sent))
        gap = next(m for m in socket.sent if m["type"] == "gap")
        assert gap["reason"] == "pipeline_overflow" and gap["dropped_messages"] == 12
        await eventually(lambda: socket.sent[-1]["type"] == "snapshot")
        reader.cancel()
        await asyncio.gather(reader, return_exceptions=True)
        await live.close()
        research.close()

    asyncio.run(scenario())


def test_session_hint_never_claims_the_market_is_open() -> None:
    new_york = ZoneInfo("America/New_York")
    assert session_hint("test")["session"] == "not_applicable"
    sunday = datetime(2026, 10, 11, 12, 0, tzinfo=new_york)
    monday = datetime(2026, 10, 12, 10, 0, tzinfo=new_york)
    assert session_hint("iex", sunday)["session"] == "closed_weekend"
    assert session_hint("iex", monday)["session"] == "regular_hours"
    assert session_hint("iex", monday.replace(hour=8))["session"] == "pre_market"
    assert session_hint("iex", monday.replace(hour=17))["session"] == "after_hours"
    assert session_hint("iex", monday.replace(hour=2))["session"] == "closed_overnight"
    assert "holidays" in session_hint("iex", monday)["note"]
