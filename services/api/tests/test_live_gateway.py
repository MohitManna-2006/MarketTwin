"""The feed gateway's state machine against a local implementation of Alpaca's protocol.

No test here touches the network beyond the loopback interface or needs a credential.
"""

from __future__ import annotations

import asyncio
from collections.abc import Iterator
from typing import Any

import pytest
from websockets.asyncio.client import connect

from markettwin_api.live_gateway import (
    FeedGateway,
    FeedLock,
    FeedState,
    Frame,
    GatewayOptions,
    backoff_delay,
    resolve_endpoint,
)
from markettwin_api.service import ServiceError

from .live_support import ThreadedMock, eventually, quote, trade

FAST = GatewayOptions(
    connect_timeout=2.0, handshake_timeout=0.5, backoff_base=0.02, backoff_max=0.1, stable_after=30
)


@pytest.fixture
def mock() -> Iterator[ThreadedMock]:
    server = ThreadedMock()
    yield server
    server.close()


class Harness:
    def __init__(
        self, mock: ThreadedMock, symbols: list[str], key: str = "mock-key", **options: Any
    ) -> None:
        self.frames: list[Frame] = []
        self.changes: list[tuple[str, str]] = []
        self.gateway = FeedGateway(
            resolve_endpoint("mock", mock.url),
            key,
            "mock-secret",
            symbols,
            on_frame=self.frames.append,
            on_change=lambda old, new, detail: self.changes.append((old.value, new.value)),
            options=GatewayOptions(**{**FAST.__dict__, **options}),
        )

    def states(self) -> list[str]:
        return [new for _, new in self.changes]

    async def reach(self, state: FeedState) -> None:
        await eventually(lambda: self.gateway.state is state, message=f"state {state}")


def test_backoff_is_bounded_exponential_and_never_tight() -> None:
    assert backoff_delay(0, 1.0, 60.0, 0.0) == 0.5
    assert backoff_delay(0, 1.0, 60.0, 0.999) < 1.0
    assert backoff_delay(3, 1.0, 60.0, 0.0) == 4.0
    assert [backoff_delay(n, 1.0, 60.0, 1.0) for n in range(8)] == [1, 2, 4, 8, 16, 32, 60, 60]
    assert backoff_delay(10_000, 1.0, 60.0, 0.0) == 30.0
    assert all(backoff_delay(n, 1.0, 60.0, 0.0) >= 0.5 for n in range(64))


def test_handshake_authenticates_subscribes_and_delivers_exact_price_text(
    mock: ThreadedMock,
) -> None:
    async def scenario() -> None:
        harness = Harness(mock, ["AAPL", "MSFT"])
        harness.gateway.start()
        await harness.reach(FeedState.CONNECTED)
        assert harness.states() == ["CONNECTING", "AUTHENTICATING", "SUBSCRIBING", "CONNECTED"]
        status = harness.gateway.status()
        assert status["subscription_status"] == "acknowledged"
        assert status["subscribed"] == {"trades": ["AAPL", "MSFT"], "quotes": ["AAPL", "MSFT"]}
        assert status["source_label"] == "FABRICATED MOCK DATA"
        assert status["test_data"] is True
        assert "mock-secret" not in repr(status)
        mock.emit([trade("AAPL", 170.10, 5, 1), quote("AAPL", 170.09, 170.11)])
        await eventually(lambda: harness.frames)
        frame = harness.frames[0]
        assert frame.messages[0]["p"] == "170.1"
        assert frame.messages[1]["bp"] == "170.09"
        assert frame.messages[0]["s"] == 5
        assert '"p":170.1' in frame.text  # the provider payload is kept verbatim
        assert frame.receipt_wall_ns > 0 and frame.receipt_monotonic_ns > 0
        await harness.gateway.stop()
        assert harness.gateway.state is FeedState.STOPPED
        assert harness.states()[-2:] == ["STOPPING", "STOPPED"]

    asyncio.run(scenario())
    received = mock.stream.received
    assert received[0] == {"action": "auth", "key": "mock-key"}
    assert received[1] == {
        "action": "subscribe",
        "trades": ["AAPL", "MSFT"],
        "quotes": ["AAPL", "MSFT"],
    }


def test_rejected_credentials_stop_the_feed_without_retrying(mock: ThreadedMock) -> None:
    async def scenario() -> None:
        harness = Harness(mock, ["AAPL"], key="wrong-key")
        # The mock feed never receives real credentials, whatever the settings hold.
        harness.gateway.endpoint = resolve_endpoint("mock", mock.url)
        mock.stream.key = "something-else"
        harness.gateway.start()
        await eventually(lambda: "STOPPED" in harness.states())
        error = harness.gateway.status()["last_error"]
        assert error["code"] == "ProviderAuthentication"
        assert error["provider_code"] == 402
        assert error["message"] == "auth failed"
        await asyncio.sleep(0.2)
        assert mock.stream.auth_attempts == 1
        assert "RECONNECTING" not in harness.states()

    asyncio.run(scenario())


def test_an_unentitled_feed_is_reported_and_never_replaced_by_another(mock: ThreadedMock) -> None:
    async def scenario() -> None:
        mock.stream.fail_auth_with = 409
        harness = Harness(mock, ["AAPL"])
        harness.gateway.start()
        await eventually(lambda: "STOPPED" in harness.states())
        status = harness.gateway.status()
        assert status["last_error"]["code"] == "ProviderEntitlement"
        assert status["feed"] == "mock"
        assert mock.stream.accepted == 1

    asyncio.run(scenario())


def test_missing_credentials_are_refused_before_any_connection(mock: ThreadedMock) -> None:
    async def scenario() -> None:
        harness = Harness(mock, ["AAPL"], key="")
        with pytest.raises(ServiceError) as error:
            harness.gateway.start()
        assert error.value.code == "ProviderNotConfigured"
        assert harness.gateway.state is FeedState.STOPPED

    asyncio.run(scenario())
    assert mock.stream.accepted == 0


def test_transport_loss_reconnects_with_backoff_and_restores_the_subscription(
    mock: ThreadedMock,
) -> None:
    async def scenario() -> None:
        harness = Harness(mock, ["AAPL", "SPY"])
        harness.gateway.start()
        await harness.reach(FeedState.CONNECTED)
        mock.drop(refuse_seconds=0.15)
        await eventually(lambda: "RECONNECTING" in harness.states())
        retrying = harness.gateway.status()
        assert retrying["subscription_status"] == "none"
        assert retrying["last_error"]["code"] in ("TransportClosed", "TransportError")
        await eventually(lambda: harness.states().count("CONNECTED") == 2)
        status = harness.gateway.status()
        assert status["reconnects"] >= 1
        assert status["connections"] == 2
        assert status["subscribed"]["trades"] == ["AAPL", "SPY"]
        assert status["last_reconnect_at"] is not None
        mock.emit([trade("SPY", 500.25, 1, 9)])
        await eventually(lambda: harness.frames)
        await harness.gateway.stop()

    asyncio.run(scenario())
    subscriptions = [item for item in mock.stream.received if item["action"] == "subscribe"]
    assert len(subscriptions) == 2 and subscriptions[0] == subscriptions[1]


def test_the_provider_connection_limit_is_handled_as_a_refusal_with_backoff(
    mock: ThreadedMock,
) -> None:
    async def scenario() -> None:
        async with connect(mock.url) as other:
            await other.recv()
            await other.send('{"action":"auth","key":"mock-key","secret":"mock-secret"}')
            await other.recv()
            harness = Harness(mock, ["AAPL"])
            harness.gateway.start()
            await eventually(lambda: "RECONNECTING" in harness.states())
            error = harness.gateway.status()["last_error"]
            assert error["code"] == "ProviderConnectionLimit"
            assert error["provider_code"] == 406
            assert "CONNECTED" not in harness.states()
        await harness.reach(FeedState.CONNECTED)
        await harness.gateway.stop()

    asyncio.run(scenario())


def test_one_process_owns_an_endpoint(mock: ThreadedMock) -> None:
    async def scenario() -> None:
        first = Harness(mock, ["AAPL"])
        second = Harness(mock, ["AAPL"])
        first.gateway.start()
        with pytest.raises(ServiceError) as error:
            second.gateway.start()
        assert error.value.code == "FeedOwnedByAnotherProcess"
        await first.gateway.stop()
        second.gateway.start()
        await second.reach(FeedState.CONNECTED)
        await second.gateway.stop()

    asyncio.run(scenario())
    lock = FeedLock(mock.url)
    lock.acquire()
    lock.release()


def test_symbol_limit_degrades_and_a_smaller_watchlist_recovers(mock: ThreadedMock) -> None:
    async def scenario() -> None:
        mock.stream.symbol_limit = 2
        harness = Harness(mock, ["AAPL", "MSFT", "SPY"])
        harness.gateway.start()
        await harness.reach(FeedState.DEGRADED)
        status = harness.gateway.status()
        assert status["impairments"][0]["code"] == "ProviderSymbolLimit"
        assert status["subscription_status"] in ("pending", "mismatch")
        assert status["subscribed"]["trades"] == []
        await harness.gateway.set_symbols(["AAPL", "MSFT"])
        await harness.reach(FeedState.CONNECTED)
        assert harness.gateway.status()["impairments"] == []
        await harness.gateway.stop()

    asyncio.run(scenario())


def test_the_watchlist_changes_by_difference_while_connected(mock: ThreadedMock) -> None:
    async def scenario() -> None:
        harness = Harness(mock, ["AAPL", "MSFT"])
        harness.gateway.start()
        await harness.reach(FeedState.CONNECTED)
        await harness.gateway.set_symbols(["MSFT", "NVDA"])
        await eventually(
            lambda: harness.gateway.status()["subscribed"]["quotes"] == ["MSFT", "NVDA"]
        )
        assert harness.gateway.state is FeedState.CONNECTED
        await harness.gateway.stop()

    asyncio.run(scenario())
    assert mock.stream.received[-2:] == [
        {"action": "unsubscribe", "trades": ["AAPL"], "quotes": ["AAPL"]},
        {"action": "subscribe", "trades": ["NVDA"], "quotes": ["NVDA"]},
    ]


def test_a_silent_provider_times_out_and_stop_cancels_cleanly(mock: ThreadedMock) -> None:
    async def scenario() -> None:
        mock.stream.silent = True
        harness = Harness(mock, ["AAPL"], handshake_timeout=0.05)
        harness.gateway.start()
        await eventually(lambda: "RECONNECTING" in harness.states())
        assert harness.gateway.status()["last_error"]["code"] == "ProviderTimeout"
        await harness.gateway.stop()
        assert harness.gateway.state is FeedState.STOPPED
        assert not harness.gateway.running
        leftover = [task for task in asyncio.all_tasks() if task is not asyncio.current_task()]
        assert leftover == []

    asyncio.run(scenario())


def test_provider_error_frames_and_malformed_frames_are_explicit(mock: ThreadedMock) -> None:
    async def scenario() -> None:
        harness = Harness(mock, ["AAPL"])
        harness.gateway.start()
        await harness.reach(FeedState.CONNECTED)
        mock.call(mock.stream.emit_raw('[{"T":"error","code":407,"msg":"slow client"}]'))
        await harness.reach(FeedState.DEGRADED)
        assert harness.gateway.status()["last_error"]["code"] == "ProviderSlowClient"
        mock.call(mock.stream.emit_raw('{"not":"a frame"}'))
        await eventually(lambda: harness.gateway.status()["protocol_errors"] == 1)
        mock.call(mock.stream.emit_raw("not json"))
        await eventually(lambda: "RECONNECTING" in harness.states())
        assert harness.gateway.status()["last_error"]["code"] == "ProviderProtocol"
        await harness.gateway.stop()

    asyncio.run(scenario())


def test_only_a_loopback_mock_and_the_two_alpaca_feeds_exist() -> None:
    assert resolve_endpoint("iex").url == "wss://stream.data.alpaca.markets/v2/iex"
    assert resolve_endpoint("test").url == "wss://stream.data.alpaca.markets/v2/test"
    assert resolve_endpoint("test").source_label == "TEST DATA"
    assert resolve_endpoint("iex").test_data is False
    for url in ("ws://example.com/v2/mock", "wss://127.0.0.1/v2/mock", ""):
        with pytest.raises(ServiceError):
            resolve_endpoint("mock", url)
    with pytest.raises(ServiceError):
        resolve_endpoint("sip")
