"""The one upstream connection to a live market-data feed, as an explicit state machine.

Acquisition only, like the historical adapter: this module speaks the provider's
WebSocket protocol, keeps the connection alive, and hands whole frames to its owner. It
never interprets a price. The native core normalizes every market number.

States and the transitions between them::

    STOPPED        -- start() --------------------------------> CONNECTING
    CONNECTING     -- transport open, provider says connected -> AUTHENTICATING
    AUTHENTICATING -- provider says authenticated ------------> SUBSCRIBING
    SUBSCRIBING    -- subscription ack equals the watchlist --> CONNECTED
    SUBSCRIBING    -- subscription ack differs, or refused ---> DEGRADED
    CONNECTED      -- provider error frame, or ack differs ---> DEGRADED
    DEGRADED       -- subscription ack equals the watchlist --> CONNECTED
    any live state -- transport lost, timeout, retryable error > RECONNECTING
    RECONNECTING   -- backoff delay elapsed -------------------> CONNECTING
    any live state -- unrecoverable provider refusal ----------> STOPPED (error kept)
    any state      -- stop() ----------------------------------> STOPPING -> STOPPED

CONNECTED describes the transport and the subscription only. Whether the market is
producing updates is a separate fact the hub derives from observation times: a healthy
connection on a weekend is CONNECTED with no market activity.

The provider's own limits are respected rather than worked around: one connection per
endpoint (guarded here by a per-user lock file as well as by the provider), a symbol
ceiling, and feed entitlement. The gateway never switches feed on its own.
"""

from __future__ import annotations

import asyncio
import contextlib
import hashlib
import json
import os
import random
import tempfile
import time
from collections import deque
from collections.abc import Awaitable, Callable
from dataclasses import dataclass, field
from decimal import Decimal
from enum import StrEnum
from pathlib import Path
from typing import Any, Protocol
from urllib.parse import urlsplit

from websockets.asyncio.client import connect as websocket_connect
from websockets.exceptions import ConnectionClosed, WebSocketException

from markettwin_api.registry import now
from markettwin_api.service import ServiceError

try:  # POSIX only; elsewhere the single-worker mode is documented rather than enforced.
    import fcntl
except ImportError:  # pragma: no cover - not reachable on the supported platforms
    fcntl = None  # type: ignore[assignment]

Json = dict[str, Any]

ALPACA_STREAM = "wss://stream.data.alpaca.markets/v2"
TEST_SYMBOL = "FAKEPACA"
_PRICE_FIELDS = ("p", "bp", "ap")
_LOOPBACK = ("127.0.0.1", "localhost", "::1")


class FeedState(StrEnum):
    STOPPED = "STOPPED"
    CONNECTING = "CONNECTING"
    AUTHENTICATING = "AUTHENTICATING"
    SUBSCRIBING = "SUBSCRIBING"
    CONNECTED = "CONNECTED"
    RECONNECTING = "RECONNECTING"
    DEGRADED = "DEGRADED"
    STOPPING = "STOPPING"


# States in which the transport is open and authenticated.
UP = (FeedState.SUBSCRIBING, FeedState.CONNECTED, FeedState.DEGRADED)

# What each provider error code means for the connection. Codes are Alpaca's, from its
# streaming documentation; the message text is the provider's and holds no credential.
_FATAL = {
    401: "ProviderAuthentication",
    402: "ProviderAuthentication",
    409: "ProviderEntitlement",
}
_RETRY = {404: "ProviderAuthTimeout", 406: "ProviderConnectionLimit", 500: "ProviderInternalError"}
_IMPAIR = {
    400: "ProviderInvalidSyntax",
    405: "ProviderSymbolLimit",
    407: "ProviderSlowClient",
    410: "ProviderInvalidSubscription",
}


@dataclass(frozen=True)
class FeedEndpoint:
    """One feed and how its data must be labelled everywhere it appears."""

    feed: str
    url: str
    provider: str
    source_label: str
    test_data: bool
    scope: str
    license: str


def resolve_endpoint(feed: str, mock_url: str = "") -> FeedEndpoint:
    if feed == "iex":
        return FeedEndpoint(
            "iex",
            f"{ALPACA_STREAM}/iex",
            "alpaca_live",
            "OBSERVED IEX DATA",
            False,
            "single_venue",
            "alpaca_terms_apply_no_redistribution",
        )
    if feed == "test":
        return FeedEndpoint(
            "test",
            f"{ALPACA_STREAM}/test",
            "alpaca_live",
            "TEST DATA",
            True,
            "provider_test_stream",
            "alpaca_test_stream_not_market_data",
        )
    if feed == "mock":
        parts = urlsplit(mock_url)
        if parts.scheme != "ws" or parts.hostname not in _LOOPBACK:
            raise ServiceError(
                422, "InvalidMockFeed", "the mock feed needs a ws:// URL on the loopback interface"
            )
        return FeedEndpoint(
            "mock",
            mock_url,
            "mock_live",
            "FABRICATED MOCK DATA",
            True,
            "fabricated_mock_stream",
            "fabricated_test_fixture",
        )
    raise ServiceError(422, "UnsupportedFeed", "live feeds are test, iex, and mock")


class ProviderRefusal(Exception):  # noqa: N818 - a provider's decision, not a program error
    def __init__(self, code: str, message: str, provider_code: int | None, fatal: bool) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.provider_code = provider_code
        self.fatal = fatal


@dataclass(frozen=True)
class Frame:
    """One provider data frame exactly as received, with both receipt clocks."""

    number: int
    text: str
    messages: list[Any]
    receipt_wall_ns: int
    receipt_monotonic_ns: int


class Connection(Protocol):
    async def send(self, message: str) -> None: ...
    async def recv(self) -> str | bytes: ...
    async def close(self) -> None: ...


Connector = Callable[[str], Awaitable[Connection]]


def render_prices(messages: list[Any]) -> None:
    """Turn each price lexeme into the decimal string the core requires.

    Frames are parsed with ``parse_float=Decimal``, so a price has not passed through
    binary floating point; ``str`` of that Decimal is the source text. Nothing is rounded
    and nothing else is touched.
    """
    for message in messages:
        if isinstance(message, dict):
            for key in _PRICE_FIELDS:
                value = message.get(key)
                if isinstance(value, Decimal) or (
                    isinstance(value, int) and not isinstance(value, bool)
                ):
                    message[key] = str(value)


class FeedLock:
    """Singleton ownership of one upstream endpoint for this operating-system user.

    A second API process, another uvicorn worker, or an autoreload child cannot open a
    second connection to the same endpoint: the provider would refuse it (error 406) and,
    worse, two processes would fight over the one permitted session.
    """

    def __init__(self, url: str) -> None:
        digest = hashlib.sha256(url.encode()).hexdigest()[:16]
        user = os.getuid() if hasattr(os, "getuid") else 0
        self.path = Path(tempfile.gettempdir()) / f"markettwin-live-{user}-{digest}.lock"
        self._handle: int | None = None

    def acquire(self) -> None:
        if fcntl is None or self._handle is not None:
            return
        handle = os.open(self.path, os.O_RDWR | os.O_CREAT, 0o600)
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            os.close(handle)
            raise ServiceError(
                409,
                "FeedOwnedByAnotherProcess",
                "another MarketTwin process on this machine owns this feed connection",
            ) from error
        self._handle = handle

    def release(self) -> None:
        if self._handle is not None:
            with contextlib.suppress(OSError):
                os.close(self._handle)
            self._handle = None


@dataclass
class GatewayOptions:
    connect_timeout: float = 10.0
    handshake_timeout: float = 10.0
    backoff_base: float = 1.0
    backoff_max: float = 60.0
    stable_after: float = 30.0
    max_symbols: int = 30


@dataclass
class _Counters:
    reconnects: int = 0
    connections: int = 0
    frames: int = 0
    data_frames: int = 0
    messages: int = 0
    protocol_errors: int = 0
    errors: deque[Json] = field(default_factory=lambda: deque(maxlen=50))


def backoff_delay(attempt: int, base: float, ceiling: float, jitter: float) -> float:
    """Bounded exponential backoff with equal jitter: half fixed, half random.

    ``jitter`` is uniform in [0, 1). The result is never below half the capped step, so a
    failing endpoint is never retried in a tight loop.
    """
    step = min(ceiling, base * (2 ** min(attempt, 30)))
    return float(step * (0.5 + 0.5 * jitter))


class FeedGateway:
    def __init__(
        self,
        endpoint: FeedEndpoint,
        key_id: str,
        secret_key: str,
        symbols: list[str],
        *,
        on_frame: Callable[[Frame], None],
        on_change: Callable[[FeedState, FeedState, str], None],
        options: GatewayOptions | None = None,
        connector: Connector | None = None,
        sleep: Callable[[float], Awaitable[None]] | None = None,
        jitter: Callable[[], float] = random.random,
    ) -> None:
        self.endpoint = endpoint
        self._key_id = key_id
        self._secret_key = secret_key
        self._desired = list(symbols)
        self._on_frame = on_frame
        self._on_change = on_change
        self.options = options or GatewayOptions()
        self._connector = connector or self._connect
        self._sleep = sleep
        self._jitter = jitter
        self._lock = FeedLock(endpoint.url)
        self.state = FeedState.STOPPED
        self._task: asyncio.Task[None] | None = None
        self._stop = asyncio.Event()
        self._connection: Connection | None = None
        self._actual: dict[str, list[str]] = {"trades": [], "quotes": []}
        self._acknowledged = False
        self._impairments: dict[str, str] = {}
        self._counters = _Counters()
        self._attempt = 0
        self._next_retry_at: float | None = None
        self._connected_since: str | None = None
        self._last_reconnect_at: str | None = None
        self._last_frame_monotonic: float | None = None
        self._last_frame_at: str | None = None
        self._last_error: Json | None = None
        self._frame_number = 0

    # ---- lifecycle ------------------------------------------------------------------------

    def start(self) -> None:
        """Take ownership of the endpoint and begin connecting. Raises without connecting
        when credentials are absent or another process owns the endpoint."""
        if self._task is not None and not self._task.done():
            return
        if not self._key_id.strip() or not self._secret_key.strip():
            raise ServiceError(
                503,
                "ProviderNotConfigured",
                "set ALPACA_API_KEY_ID and ALPACA_API_SECRET_KEY in the API's environment",
            )
        if any(character.isspace() for character in self._key_id + self._secret_key):
            raise ServiceError(503, "ProviderNotConfigured", "Alpaca credentials are malformed")
        self._lock.acquire()
        self._stop = asyncio.Event()
        self._attempt = 0
        self._last_error = None
        self._task = asyncio.create_task(self._run(), name=f"live-gateway-{self.endpoint.feed}")

    async def stop(self) -> None:
        task = self._task
        if task is None:
            self._lock.release()
            return
        if not task.done():
            self._set(FeedState.STOPPING, "stop requested")
            self._stop.set()
            connection = self._connection
            if connection is not None:
                with contextlib.suppress(Exception):
                    await asyncio.wait_for(connection.close(), 2.0)
            try:
                await asyncio.wait_for(asyncio.shield(task), 5.0)
            except (TimeoutError, asyncio.CancelledError):
                task.cancel()
                with contextlib.suppress(asyncio.CancelledError, Exception):
                    await task
        self._task = None
        self._lock.release()
        if self.state is not FeedState.STOPPED:
            self._set(FeedState.STOPPED, "stopped")

    @property
    def running(self) -> bool:
        return self._task is not None and not self._task.done()

    async def set_symbols(self, symbols: list[str]) -> None:
        """Replace the watchlist. While connected only the difference is sent; the
        provider's acknowledgment, not this request, decides the resulting state."""
        self._desired = list(symbols)
        if self.state in UP and self._connection is not None:
            await self._reconcile(self._connection)

    # ---- the connection loop --------------------------------------------------------------

    async def _run(self) -> None:
        reason = "stopped"
        try:
            while not self._stop.is_set():
                opened = time.monotonic()
                established = False
                try:
                    self._set(FeedState.CONNECTING, "opening transport")
                    connection = await asyncio.wait_for(
                        self._connector(self.endpoint.url), self.options.connect_timeout
                    )
                    self._connection = connection
                    try:
                        await self._handshake(connection)
                        established = True
                        self._counters.connections += 1
                        self._connected_since = now()
                        await self._read(connection)
                    finally:
                        self._connection = None
                        with contextlib.suppress(Exception):
                            await asyncio.wait_for(connection.close(), 2.0)
                except ProviderRefusal as refusal:
                    self._record_error(refusal.code, refusal.message, refusal.provider_code)
                    if refusal.fatal:
                        reason = refusal.message
                        return
                except TimeoutError:
                    self._record_error("ProviderTimeout", "the provider did not answer in time")
                except (OSError, WebSocketException) as error:
                    # The exception text may name the endpoint; its class is enough here.
                    closed = isinstance(error, ConnectionClosed)
                    self._record_error(
                        "TransportClosed" if closed else "TransportError", type(error).__name__
                    )
                except json.JSONDecodeError:
                    self._counters.protocol_errors += 1
                    self._record_error("ProviderProtocol", "the provider sent a non-JSON frame")
                if self._stop.is_set():
                    return
                if established and time.monotonic() - opened >= self.options.stable_after:
                    self._attempt = 0
                delay = backoff_delay(
                    self._attempt,
                    self.options.backoff_base,
                    self.options.backoff_max,
                    self._jitter(),
                )
                self._attempt += 1
                self._counters.reconnects += 1
                self._last_reconnect_at = now()
                self._next_retry_at = time.monotonic() + delay
                self._acknowledged = False
                self._actual = {"trades": [], "quotes": []}
                self._set(FeedState.RECONNECTING, f"retry {self._attempt} in {delay:.1f} s")
                await self._wait(delay)
                self._next_retry_at = None
        finally:
            self._connection = None
            self._connected_since = None
            self._next_retry_at = None
            self._acknowledged = False
            self._actual = {"trades": [], "quotes": []}
            self._lock.release()
            self._set(FeedState.STOPPED, reason)

    async def _wait(self, delay: float) -> None:
        if self._sleep is not None:
            await self._sleep(delay)
            return
        with contextlib.suppress(TimeoutError):
            await asyncio.wait_for(self._stop.wait(), delay)

    async def _connect(self, url: str) -> Connection:
        connection: Connection = await websocket_connect(
            url,
            open_timeout=self.options.connect_timeout,
            ping_interval=20,
            ping_timeout=20,
            close_timeout=2,
            max_size=8 * 1024 * 1024,
            max_queue=64,
        )
        return connection

    async def _control(self, connection: Connection, expected: str) -> None:
        """Wait for one ``success`` control frame carrying ``expected``."""
        raw = await asyncio.wait_for(connection.recv(), self.options.handshake_timeout)
        self._touch()
        frame = json.loads(raw, parse_float=Decimal) if isinstance(raw, str) else None
        first = frame[0] if isinstance(frame, list) and frame and isinstance(frame[0], dict) else {}
        if first.get("T") == "error":
            raise self._refusal(first)
        if first.get("T") != "success" or first.get("msg") != expected:
            self._counters.protocol_errors += 1
            raise ProviderRefusal(
                "ProviderProtocol", f"expected the provider to say {expected}", None, False
            )

    async def _handshake(self, connection: Connection) -> None:
        await self._control(connection, "connected")
        self._set(FeedState.AUTHENTICATING, "transport open")
        # The mock feed is a local fixture. Real credentials are never sent to it.
        key, secret = (
            ("mock-key", "mock-secret")
            if self.endpoint.feed == "mock"
            else (self._key_id, self._secret_key)
        )
        await connection.send(json.dumps({"action": "auth", "key": key, "secret": secret}))
        await self._control(connection, "authenticated")
        self._impairments.clear()
        self._acknowledged = False
        self._actual = {"trades": [], "quotes": []}
        self._set(FeedState.SUBSCRIBING, "authenticated")
        if self._desired:
            await self._reconcile(connection)
        else:
            self._acknowledged = True
            self._set(FeedState.CONNECTED, "authenticated with an empty watchlist")

    async def _reconcile(self, connection: Connection) -> None:
        desired = set(self._desired)
        for action, pick in (
            ("unsubscribe", lambda current: sorted(set(current) - desired)),
            ("subscribe", lambda current: sorted(desired - set(current))),
        ):
            request = {
                channel: symbols
                for channel in ("trades", "quotes")
                if (symbols := pick(self._actual[channel]))
            }
            if request:
                self._acknowledged = False
                await connection.send(json.dumps({"action": action, **request}))

    async def _read(self, connection: Connection) -> None:
        while not self._stop.is_set():
            if self.state is FeedState.SUBSCRIBING:
                raw = await asyncio.wait_for(connection.recv(), self.options.handshake_timeout)
            else:
                raw = await connection.recv()
            wall, monotonic = time.time_ns(), time.monotonic_ns()
            self._touch()
            if not isinstance(raw, str):
                self._counters.protocol_errors += 1
                continue
            frame = json.loads(raw, parse_float=Decimal)
            if not isinstance(frame, list) or not frame:
                self._counters.protocol_errors += 1
                continue
            first = frame[0]
            kind = first.get("T") if isinstance(first, dict) else None
            if kind == "error":
                refusal = self._refusal(first)
                if refusal.fatal or refusal.provider_code in _RETRY:
                    raise refusal
                self._record_error(refusal.code, refusal.message, refusal.provider_code)
                self._impairments[refusal.code] = refusal.message
                self._set(FeedState.DEGRADED, refusal.message)
            elif kind == "subscription":
                self._acknowledge(first)
            elif kind == "success":
                continue
            else:
                render_prices(frame)
                self._frame_number += 1
                self._counters.data_frames += 1
                self._counters.messages += len(frame)
                self._on_frame(Frame(self._frame_number, raw, frame, wall, monotonic))

    def _acknowledge(self, message: Json) -> None:
        self._actual = {
            channel: sorted(str(symbol) for symbol in message.get(channel) or [])
            for channel in ("trades", "quotes")
        }
        self._acknowledged = True
        desired = sorted(self._desired)
        if self._actual["trades"] == desired and self._actual["quotes"] == desired:
            self._impairments.clear()
            self._set(FeedState.CONNECTED, "subscription acknowledged")
            return
        missing = sorted(
            (set(desired) - set(self._actual["trades"]))
            | (set(desired) - set(self._actual["quotes"]))
        )
        detail = "the provider's subscription differs from the watchlist"
        if missing:
            detail += f"; not subscribed: {', '.join(missing)}"
        self._impairments["SubscriptionMismatch"] = detail
        self._set(FeedState.DEGRADED, detail)

    def _refusal(self, message: Json) -> ProviderRefusal:
        raw_code = message.get("code")
        code = int(raw_code) if isinstance(raw_code, int | Decimal) else None
        text = str(message.get("msg") or "provider error")[:200]
        if code in _FATAL:
            return ProviderRefusal(_FATAL[code], text, code, True)
        if code in _RETRY:
            return ProviderRefusal(_RETRY[code], text, code, False)
        return ProviderRefusal(_IMPAIR.get(code or 0, "ProviderError"), text, code, False)

    # ---- bookkeeping ----------------------------------------------------------------------

    def _touch(self) -> None:
        self._counters.frames += 1
        self._last_frame_monotonic = time.monotonic()
        self._last_frame_at = now()

    def _record_error(self, code: str, message: str, provider_code: int | None = None) -> None:
        self._last_error = {
            "code": code,
            "message": message,
            "provider_code": provider_code,
            "at": now(),
        }
        self._counters.errors.append(self._last_error)

    def _set(self, state: FeedState, detail: str) -> None:
        previous = self.state
        if previous is state:
            return
        self.state = state
        self._on_change(previous, state, detail)

    def status(self) -> Json:
        desired = sorted(self._desired)
        if not self._acknowledged:
            subscription = "pending" if self.state in UP else "none"
        elif self._actual["trades"] == desired and self._actual["quotes"] == desired:
            subscription = "acknowledged"
        else:
            subscription = "mismatch"
        return {
            "state": self.state.value,
            "feed": self.endpoint.feed,
            "provider": self.endpoint.provider,
            "source_label": self.endpoint.source_label,
            "test_data": self.endpoint.test_data,
            "scope": self.endpoint.scope,
            "endpoint_host": urlsplit(self.endpoint.url).hostname,
            "desired_symbols": desired,
            "subscribed": dict(self._actual),
            "subscription_status": subscription,
            "impairments": [
                {"code": code, "message": message} for code, message in self._impairments.items()
            ],
            "reconnects": self._counters.reconnects,
            "connections": self._counters.connections,
            "attempt": self._attempt,
            "next_retry_in_ms": None
            if self._next_retry_at is None
            else max(0, round((self._next_retry_at - time.monotonic()) * 1000)),
            "connected_since": self._connected_since if self.state in UP else None,
            "last_reconnect_at": self._last_reconnect_at,
            "last_message_at": self._last_frame_at,
            "last_message_age_ms": None
            if self._last_frame_monotonic is None
            else round((time.monotonic() - self._last_frame_monotonic) * 1000),
            "frames": self._counters.frames,
            "data_frames": self._counters.data_frames,
            "messages": self._counters.messages,
            "protocol_errors": self._counters.protocol_errors,
            "last_error": self._last_error,
            "recent_errors": list(self._counters.errors),
        }
