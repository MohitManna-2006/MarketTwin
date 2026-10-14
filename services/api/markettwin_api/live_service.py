"""The live market-data hub: one upstream feed, native normalization, many local readers.

    provider WebSocket -> FeedGateway -> capture file (if recording)
                                      -> bounded inbox -> one native batch call
                                      -> per-client bounded queues -> browser WebSockets

Everything here runs on the API's event loop. Three resources are bounded and each has an
explicit, counted, and announced overflow: the inbox between the gateway and the core
(frames are dropped and every client is told), each client's queue (that client alone is
told it has a gap and is given a fresh snapshot), and the capture file (recording stops
and says why). Nothing grows without limit and no loss is silent.

Stream protocol, version 1. Every server message is one JSON object with ``v``, ``type``,
and ``stream``. ``seq`` numbers the messages this hub has published since the API process
started; it is not a provider or exchange sequence, and the provider supplies none.

    hello     protocol limits; first message on every connection
    snapshot  complete state as of ``seq``: status, every instrument, recent events
    events    normalized events since the previous message, and the states they changed
    status    feed, market, pipeline, client, and recording status; every instrument's
              freshness (once per heartbeat and on every feed state change)
    gap       this client, or the pipeline, lost messages; a snapshot follows
    pong      answer to a client ``ping``

A client that sees ``seq`` skip sends ``{"type":"resync"}`` and receives a snapshot.
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import re
import secrets
import time
from collections import deque
from collections.abc import Awaitable, Callable
from dataclasses import dataclass, field
from datetime import datetime
from typing import Any, Protocol
from zoneinfo import ZoneInfo

import markettwin

from markettwin_api.historical_service import HistoricalResearch
from markettwin_api.live_gateway import (
    TEST_SYMBOL,
    Connector,
    FeedEndpoint,
    FeedGateway,
    FeedState,
    Frame,
    GatewayOptions,
    resolve_endpoint,
)
from markettwin_api.live_recording import (
    DISCLAIMER,
    RECORDING_ID,
    CaptureWriter,
    encode_batch,
    finalize_capture,
    iso,
)
from markettwin_api.registry import Row, now
from markettwin_api.service import Research, ServiceError

Json = dict[str, Any]

PROTOCOL_VERSION = 1
SYMBOL = re.compile(r"[A-Z][A-Z.]{0,11}\Z")
_DELIVERING = (FeedState.CONNECTED, FeedState.DEGRADED)
_NEW_YORK = ZoneInfo("America/New_York")


class ClientSocket(Protocol):
    async def send_text(self, data: str) -> None: ...
    async def receive_text(self) -> str: ...
    async def close(self, code: int = 1000, reason: str | None = None) -> None: ...


@dataclass(eq=False)
class _Client:
    queue: deque[tuple[str, int]] = field(default_factory=deque)
    wake: asyncio.Event = field(default_factory=asyncio.Event)
    lagging: bool = False
    resync: bool = False
    closed: bool = False
    dropped_messages: int = 0
    dropped_events: int = 0


@dataclass
class _Recording:
    id: str
    writer: CaptureWriter
    started_at: str
    upstream_up: bool
    stopping: bool = False
    gaps: int = 0
    last_sync: float = field(default_factory=time.monotonic)


def session_hint(feed: str, moment: datetime | None = None) -> Json:
    """Where the wall clock sits in the US equity day. A hint for reading silence, never a
    statement that the market is open: holidays and early closes are not known here."""
    if feed != "iex":
        return {"session": "not_applicable", "note": "this feed does not follow market hours"}
    local = (moment or datetime.now(_NEW_YORK)).astimezone(_NEW_YORK)
    minute = local.hour * 60 + local.minute
    if local.weekday() >= 5:
        session = "closed_weekend"
    elif 570 <= minute < 960:
        session = "regular_hours"
    elif 240 <= minute < 570:
        session = "pre_market"
    elif 960 <= minute < 1200:
        session = "after_hours"
    else:
        session = "closed_overnight"
    return {
        "session": session,
        "note": "from the New York wall clock only; exchange holidays and early closes are "
        "not known, and IEX publishes little outside regular hours",
    }


def _percentile(ordered: list[int], fraction: float) -> int | None:
    if not ordered:
        return None
    return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))]


def _micros(nanoseconds: int | None) -> int | None:
    return None if nanoseconds is None else nanoseconds // 1000


class LiveService:
    def __init__(
        self,
        research: Research,
        historical: HistoricalResearch,
        *,
        connector: Connector | None = None,
        sleep: Callable[[float], Awaitable[None]] | None = None,
    ) -> None:
        self.research = research
        self.historical = historical
        self.settings = research.settings
        self.registry = research.registry
        self._connector = connector
        self._sleep = sleep
        self.recordings_dir = self.settings.live_dir / "recordings"
        self.recordings_dir.mkdir(parents=True, exist_ok=True)
        self._watchlists: dict[str, list[str]] = {
            "iex": [item for item in self.settings.live_symbols if SYMBOL.fullmatch(item)],
            "test": [TEST_SYMBOL],
            "mock": [TEST_SYMBOL],
        }
        self._feed = (
            self.settings.live_feed if self.settings.live_feed in self._watchlists else "iex"
        )
        self._endpoint: FeedEndpoint | None = None
        self._gateway: FeedGateway | None = None
        self._normalizer: markettwin.LiveNormalizer | None = None
        self._stream = secrets.token_hex(6)
        self._seq = 0
        self._states: dict[str, Json] = {}
        self._recent: dict[str, deque[Json]] = {}
        self._inbox: deque[Frame] = deque()
        self._inbox_ready = asyncio.Event()
        self._clients: set[_Client] = set()
        self._recording: _Recording | None = None
        self._finalizing: set[asyncio.Task[None]] = set()
        self._tasks: list[asyncio.Task[None]] = []
        self._status_dirty = asyncio.Event()
        self._start_error: Json | None = None
        self._last_observation_monotonic: float | None = None
        self._last_observation_wall_ns: int | None = None
        self._latency_ns: deque[int] = deque(maxlen=2048)
        self._native_ns_per_message: deque[int] = deque(maxlen=2048)
        self._feed_events: deque[Json] = deque(maxlen=50)
        self._rejects: deque[Json] = deque(maxlen=50)
        self._counts = {
            "batches": 0,
            "frames": 0,
            "messages": 0,
            "events": 0,
            "rejected": 0,
            "dropped_frames": 0,
            "dropped_messages": 0,
            "inbox_high_water": 0,
            "client_dropped_messages": 0,
            "client_dropped_events": 0,
            "client_overflows": 0,
            "slow_disconnects": 0,
            "refused_clients": 0,
            "client_queue_high_water": 0,
        }
        self._unannounced_drops = 0
        self._rate_mark = (time.monotonic(), 0, 0)
        self._rates = {"messages_per_s": 0.0, "events_per_s": 0.0}

    # ---- lifecycle ------------------------------------------------------------------------

    async def startup(self) -> None:
        self._tasks = [
            asyncio.create_task(self._pump(), name="live-pump"),
            asyncio.create_task(self._heartbeat(), name="live-heartbeat"),
        ]
        for row in self.registry.unfinished_live_recordings():
            # A capture belongs to the process that started it, so this one was interrupted.
            self.registry.update_live_recording(row["id"], status="finalizing")
            self._spawn_finalize(row["id"])
        if self.settings.live_autostart:
            try:
                await self.start_feed(None)
            except ServiceError as error:
                # A feed that cannot start must not stop the rest of the API from serving.
                self._start_error = {"code": error.code, "message": error.message, "at": now()}

    async def close(self) -> None:
        if self._recording is not None:
            await self.stop_recording("service_shutdown")
        if self._gateway is not None:
            await self._gateway.stop()
        for task in self._tasks:
            task.cancel()
        for task in self._tasks:
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await task
        self._tasks = []
        if self._finalizing:
            await asyncio.gather(*self._finalizing, return_exceptions=True)
        for client in list(self._clients):
            client.closed = True
            client.wake.set()

    async def settle(self) -> None:
        """Wait until no recording is being finalized."""
        while self._finalizing:
            await asyncio.gather(*list(self._finalizing), return_exceptions=True)

    # ---- feed control ---------------------------------------------------------------------

    async def start_feed(self, feed: str | None) -> Json:
        selected = (feed or self._feed).strip().lower()
        gateway = self._gateway
        if gateway is not None and gateway.running:
            if gateway.endpoint.feed == selected:
                return self.status()
            raise ServiceError(
                409,
                "FeedAlreadyRunning",
                f"the {gateway.endpoint.feed} feed is running; stop it before starting another",
            )
        endpoint = resolve_endpoint(selected, self.settings.live_mock_url)
        settings = self.settings
        gateway = FeedGateway(
            endpoint,
            settings.alpaca_key_id if endpoint.feed != "mock" else "mock-key",
            settings.alpaca_secret_key if endpoint.feed != "mock" else "mock-secret",
            list(self._watchlists[endpoint.feed]),
            on_frame=self._on_frame,
            on_change=self._on_change,
            options=GatewayOptions(
                connect_timeout=settings.live_connect_timeout_seconds,
                handshake_timeout=settings.live_handshake_timeout_seconds,
                backoff_base=settings.live_backoff_base_seconds,
                backoff_max=settings.live_backoff_max_seconds,
                stable_after=settings.live_stable_after_seconds,
                max_symbols=settings.live_max_symbols,
            ),
            connector=self._connector,
            sleep=self._sleep,
        )
        normalizer = markettwin.LiveNormalizer(
            {
                "provider": endpoint.provider,
                "feed": endpoint.feed,
                "stale_after_ns": int(settings.live_stale_after_seconds * 1e9),
                "skew_alert_ns": int(settings.live_skew_alert_seconds * 1e9),
                "max_symbols": max(1, settings.live_max_symbols),
            }
        )
        gateway.start()  # raises before anything is replaced
        self._feed = endpoint.feed
        self._endpoint = endpoint
        self._gateway = gateway
        self._normalizer = normalizer
        self._start_error = None
        self._states = {}
        self._recent = {}
        self._inbox.clear()
        self._last_observation_monotonic = None
        self._last_observation_wall_ns = None
        self._stream = secrets.token_hex(6)
        # A new feed is a new stream: every reader starts again from a snapshot.
        for client in self._clients:
            client.queue.clear()
            client.resync = True
            client.wake.set()
        return self.status()

    async def stop_feed(self) -> Json:
        if self._recording is not None:
            await self.stop_recording("feed_stopped")
        if self._gateway is not None:
            await self._gateway.stop()
        self._publish("status", self._status_payload())
        return self.status()

    def symbols(self) -> Json:
        endpoint = self._endpoint
        gateway = self._gateway
        return {
            "feed": self._feed,
            "symbols": list(self._watchlists[self._feed]),
            "max_symbols": self.settings.live_max_symbols,
            "editable": self._feed != "test",
            "subscribed": gateway.status()["subscribed"]
            if gateway
            else {"trades": [], "quotes": []},
            "subscription_status": gateway.status()["subscription_status"] if gateway else "none",
            "source_label": endpoint.source_label if endpoint else None,
            "watchlists": {feed: list(items) for feed, items in self._watchlists.items()},
        }

    async def set_symbols(self, symbols: list[str], feed: str | None = None) -> Json:
        feed = (feed or self._feed).strip().lower()
        running = self._gateway is not None and self._gateway.running
        if feed not in self._watchlists:
            raise ServiceError(422, "UnsupportedFeed", "live feeds are test, iex, and mock")
        if running and self._gateway is not None and self._gateway.endpoint.feed != feed:
            raise ServiceError(
                409, "FeedAlreadyRunning", "stop the running feed before editing another watchlist"
            )
        cleaned = list(dict.fromkeys(item.strip().upper() for item in symbols))
        if any(SYMBOL.fullmatch(item) is None for item in cleaned):
            raise ServiceError(
                422, "InvalidSymbol", "symbols are 1 to 12 upper-case letters or dots"
            )
        if len(cleaned) > self.settings.live_max_symbols:
            raise ServiceError(
                422,
                "SymbolLimitExceeded",
                f"this service subscribes to at most {self.settings.live_max_symbols} symbols; "
                "raise MARKETTWIN_LIVE_MAX_SYMBOLS only if the provider plan allows more",
            )
        if feed == "test" and cleaned != [TEST_SYMBOL]:
            raise ServiceError(
                422, "TestFeedSymbol", f"the Alpaca test stream carries only {TEST_SYMBOL}"
            )
        self._watchlists[feed] = cleaned
        if not running:
            self._feed = feed
            self._status_dirty.set()
            return self.symbols()
        if self._normalizer is not None:
            self._normalizer.retain_symbols(cleaned)
        for symbol in [item for item in self._states if item not in cleaned]:
            del self._states[symbol]
            self._recent.pop(symbol, None)
        gateway = self._gateway
        if gateway is not None:
            if self._recording is not None:
                self._recording.writer.lifecycle("watchlist", symbols=cleaned)
            await gateway.set_symbols(cleaned)
        for client in self._clients:
            client.resync = True
            client.wake.set()
        return self.symbols()

    # ---- gateway callbacks ----------------------------------------------------------------

    def _on_frame(self, frame: Frame) -> None:
        recording = self._recording
        if recording is not None and not recording.stopping:
            # The capture is written first: nothing downstream can lose it a frame.
            reason = recording.writer.frame(frame)
            if reason is not None:
                recording.stopping = True
                self._background(self.stop_recording(reason))
        if len(self._inbox) >= self.settings.live_inbox_frames:
            self._counts["dropped_frames"] += 1
            self._counts["dropped_messages"] += len(frame.messages)
            self._unannounced_drops += len(frame.messages)
            return
        self._inbox.append(frame)
        self._counts["inbox_high_water"] = max(self._counts["inbox_high_water"], len(self._inbox))
        self._inbox_ready.set()

    def _on_change(self, previous: FeedState, state: FeedState, detail: str) -> None:
        self._feed_events.append(
            {"at": now(), "from_state": previous.value, "to_state": state.value, "detail": detail}
        )
        recording = self._recording
        if recording is not None and not recording.stopping:
            recording.writer.lifecycle(
                "state", from_state=previous.value, to_state=state.value, detail=detail
            )
            delivering = state in _DELIVERING
            if recording.upstream_up and not delivering:
                recording.upstream_up = False
                recording.gaps += 1
                recording.writer.lifecycle("upstream_lost", detail=f"feed became {state.value}")
            elif not recording.upstream_up and delivering:
                recording.upstream_up = True
                recording.writer.lifecycle("upstream_restored", detail=detail)
        self._status_dirty.set()

    def _background(self, work: Awaitable[Any]) -> None:
        async def run() -> None:
            with contextlib.suppress(ServiceError):
                await work

        task = asyncio.create_task(run())
        self._finalizing.add(task)
        task.add_done_callback(self._finalizing.discard)

    # ---- the pipeline ---------------------------------------------------------------------

    async def _pump(self) -> None:
        while True:
            await self._inbox_ready.wait()
            # Frames that arrive within one short window share one native call.
            await asyncio.sleep(self.settings.live_flush_interval_seconds)
            self._inbox_ready.clear()
            while self._inbox:
                frames: list[Frame] = []
                messages = 0
                while self._inbox and messages < self.settings.live_max_batch_messages:
                    frame = self._inbox.popleft()
                    frames.append(frame)
                    messages += len(frame.messages)
                try:
                    self._process(frames, messages)
                except Exception as error:
                    # One bad batch must not end the stream. It is counted and shown.
                    self._counts["rejected"] += messages
                    self._rejects.append(
                        {"at": now(), "code": type(error).__name__, "reason": "batch refused"}
                    )
                await asyncio.sleep(0)

    def _process(self, frames: list[Frame], messages: int) -> None:
        normalizer = self._normalizer
        if normalizer is None:
            return
        result = normalizer.process_batch(
            encode_batch(
                [
                    (
                        frame.number,
                        frame.receipt_wall_ns,
                        frame.receipt_monotonic_ns,
                        frame.messages,
                    )
                    for frame in frames
                ]
            )
        )
        events: list[Json] = result["events"]
        self._counts["batches"] += 1
        self._counts["frames"] += len(frames)
        self._counts["messages"] += messages
        self._counts["events"] += len(events)
        self._counts["rejected"] += len(result["rejects"])
        if messages:
            self._native_ns_per_message.append(normalizer.last_batch_elapsed_ns // messages)
        for reject in result["rejects"]:
            self._rejects.append({"at": now(), **reject})
        self._states.update(result["states"])
        limit = self.settings.live_recent_events_per_symbol
        for event in events:
            ring = self._recent.get(event["symbol"])
            if ring is None:
                ring = self._recent[event["symbol"]] = deque(maxlen=limit)
            ring.append(event)
        if events:
            self._last_observation_monotonic = time.monotonic()
            self._last_observation_wall_ns = frames[-1].receipt_wall_ns
        self._announce_drops()
        if events or result["rejects"]:
            self._publish(
                "events",
                {"events": events, "states": result["states"], "rejects": len(result["rejects"])},
                len(events),
            )
        done = time.monotonic_ns()
        for frame in frames:
            self._latency_ns.append(done - frame.receipt_monotonic_ns)

    def _announce_drops(self) -> None:
        if self._unannounced_drops:
            dropped, self._unannounced_drops = self._unannounced_drops, 0
            self._publish(
                "gap",
                {
                    "reason": "pipeline_overflow",
                    "scope": "all_clients",
                    "dropped_messages": dropped,
                    "detail": "the service could not keep up with the provider; "
                    "these messages were not normalized",
                },
            )
            for client in self._clients:
                client.resync = True
                client.wake.set()

    async def _heartbeat(self) -> None:
        interval = self.settings.live_heartbeat_seconds
        while True:
            with contextlib.suppress(TimeoutError):
                await asyncio.wait_for(self._status_dirty.wait(), interval)
            self._status_dirty.clear()
            recording = self._recording
            if recording is not None and not recording.stopping:
                if recording.writer.expired():
                    recording.stopping = True
                    self._background(self.stop_recording("duration_limit"))
                elif (
                    time.monotonic() - recording.last_sync
                    >= self.settings.live_recording_sync_seconds
                ):
                    recording.last_sync = time.monotonic()
                    with contextlib.suppress(OSError, ValueError):
                        recording.writer.sync()
            self._refresh_states()
            mark, messages, events = self._rate_mark
            elapsed = time.monotonic() - mark
            if elapsed >= interval * 0.9:
                self._rates = {
                    "messages_per_s": round((self._counts["messages"] - messages) / elapsed, 1),
                    "events_per_s": round((self._counts["events"] - events) / elapsed, 1),
                }
                self._rate_mark = (
                    time.monotonic(),
                    self._counts["messages"],
                    self._counts["events"],
                )
            if self._clients:
                self._publish("status", self._status_payload())

    def _refresh_states(self) -> None:
        if self._normalizer is not None:
            snapshot = self._normalizer.snapshot(time.time_ns(), time.monotonic_ns())
            self._states = snapshot["states"]

    # ---- fan-out --------------------------------------------------------------------------

    def _envelope(self, kind: str, payload: Json) -> str:
        return json.dumps(
            {
                "v": PROTOCOL_VERSION,
                "type": kind,
                "stream": self._stream,
                "seq": self._seq,
                "server_wall_ns": str(time.time_ns()),
                **payload,
            },
            separators=(",", ":"),
        )

    def _publish(self, kind: str, payload: Json, events: int = 0) -> None:
        self._seq += 1
        if not self._clients:
            return
        text = self._envelope(kind, payload)
        capacity = self.settings.live_client_queue_messages
        for client in self._clients:
            if client.lagging:
                client.dropped_messages += 1
                client.dropped_events += events
                self._counts["client_dropped_messages"] += 1
                self._counts["client_dropped_events"] += events
                continue
            if len(client.queue) >= capacity:
                # A reader this far behind gains nothing from old deltas. Its queue is
                # emptied, the loss is counted, and it is resynchronized from a snapshot.
                lost_events = sum(count for _, count in client.queue) + events
                lost_messages = len(client.queue) + 1
                client.queue.clear()
                client.lagging = True
                client.dropped_messages += lost_messages
                client.dropped_events += lost_events
                self._counts["client_dropped_messages"] += lost_messages
                self._counts["client_dropped_events"] += lost_events
                self._counts["client_overflows"] += 1
            else:
                client.queue.append((text, events))
                self._counts["client_queue_high_water"] = max(
                    self._counts["client_queue_high_water"], len(client.queue)
                )
            client.wake.set()

    def _snapshot_payload(self) -> Json:
        self._refresh_states()
        recent = sorted(
            (event for ring in self._recent.values() for event in ring),
            key=lambda event: event["live_sequence"],
        )
        return {
            **self._status_payload(),
            "recent": recent,
            "watchlist": list(self._watchlists[self._feed]),
        }

    def snapshot(self) -> Json:
        document: Json = json.loads(self._envelope("snapshot", self._snapshot_payload()))
        return document

    async def serve(self, socket: ClientSocket) -> None:
        """Serve one reader until it leaves. The caller has accepted the connection."""
        if len(self._clients) >= self.settings.live_max_clients:
            self._counts["refused_clients"] += 1
            await socket.close(1013, "the live stream has no free reader slot")
            return
        client = _Client()
        self._clients.add(client)
        reader = asyncio.create_task(self._read_client(socket, client))
        try:
            await self._send(
                socket,
                self._envelope(
                    "hello",
                    {
                        "protocol": PROTOCOL_VERSION,
                        "limits": {
                            "client_queue_messages": self.settings.live_client_queue_messages,
                            "recent_events_per_symbol": self.settings.live_recent_events_per_symbol,
                            "heartbeat_ms": round(self.settings.live_heartbeat_seconds * 1000),
                        },
                        "sequence_domain": "messages published by this hub since the API "
                        "process started; not a provider or exchange sequence",
                    },
                ),
            )
            client.resync = True
            client.wake.set()
            while True:
                await client.wake.wait()
                client.wake.clear()
                if client.closed:
                    return
                while client.queue and not client.lagging:
                    text, _ = client.queue.popleft()
                    await self._send(socket, text)
                if client.lagging or client.resync:
                    # No await between clearing the flags and taking the snapshot: every
                    # message published after this point is queued behind it.
                    gap = None
                    if client.lagging:
                        gap = self._envelope(
                            "gap",
                            {
                                "reason": "slow_client",
                                "scope": "this_client",
                                "dropped_messages": client.dropped_messages,
                                "dropped_events": client.dropped_events,
                            },
                        )
                    client.lagging = client.resync = False
                    client.queue.clear()
                    snapshot = self._envelope("snapshot", self._snapshot_payload())
                    if gap is not None:
                        await self._send(socket, gap)
                    await self._send(socket, snapshot)
        except TimeoutError:
            self._counts["slow_disconnects"] += 1
            with contextlib.suppress(Exception):
                await socket.close(1013, "the reader did not accept data in time")
        except Exception:
            pass  # a reader that has gone away needs no report
        finally:
            client.closed = True
            self._clients.discard(client)
            reader.cancel()
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await reader

    async def _send(self, socket: ClientSocket, text: str) -> None:
        await asyncio.wait_for(socket.send_text(text), self.settings.live_send_timeout_seconds)

    async def _read_client(self, socket: ClientSocket, client: _Client) -> None:
        try:
            while True:
                text = await socket.receive_text()
                if len(text) > 256:
                    continue
                try:
                    message = json.loads(text)
                except ValueError:
                    continue
                kind = message.get("type") if isinstance(message, dict) else None
                if kind == "resync":
                    client.resync = True
                elif (
                    kind == "ping" and len(client.queue) < self.settings.live_client_queue_messages
                ):
                    client.queue.append((self._envelope("pong", {}), 0))
                client.wake.set()
        except asyncio.CancelledError:
            raise
        except Exception:
            client.closed = True
            client.wake.set()

    # ---- recording ------------------------------------------------------------------------

    async def start_recording(self) -> Json:
        gateway, endpoint = self._gateway, self._endpoint
        if self._recording is not None:
            raise ServiceError(409, "RecordingActive", "a recording is already in progress")
        if gateway is None or endpoint is None or not gateway.running:
            raise ServiceError(409, "FeedNotRunning", "start the live feed before recording it")
        recording_id = "rec-" + secrets.token_hex(5)
        started = time.time_ns()
        settings = self.settings
        writer = CaptureWriter(
            self.recordings_dir / recording_id,
            {
                "recording_id": recording_id,
                "provider": endpoint.provider,
                "feed": endpoint.feed,
                "source_label": endpoint.source_label,
                "test_data": endpoint.test_data,
                "scope": endpoint.scope,
                "license": endpoint.license,
                "endpoint_host": gateway.status()["endpoint_host"],
                "symbols": list(self._watchlists[endpoint.feed]),
                "started_wall_ns": str(started),
                "started_at": iso(started),
                "engine_version": self.research.build["engine_version"],
                "git_revision": self.research.build["git_revision"],
                "live_schema": self.research.build["live_schema"],
            },
            settings.live_recording_max_bytes,
            settings.live_recording_max_messages,
            settings.live_recording_max_seconds,
        )
        self.registry.insert_live_recording(
            {
                "id": recording_id,
                "provider": endpoint.provider,
                "feed": endpoint.feed,
                "status": "recording",
                "created_at": iso(started),
            }
        )
        delivering = gateway.state in _DELIVERING
        recording = _Recording(recording_id, writer, iso(started), delivering)
        if not delivering:
            recording.gaps += 1
            writer.lifecycle(
                "upstream_lost", detail=f"recording started while the feed was {gateway.state}"
            )
        self._recording = recording
        self._status_dirty.set()
        return self.recording(recording_id)

    async def stop_recording(self, reason: str = "user_stopped") -> Json:
        recording = self._recording
        if recording is None:
            raise ServiceError(409, "NoActiveRecording", "no recording is in progress")
        self._recording = None
        recording.stopping = True
        recording.writer.close(reason)
        self.registry.update_live_recording(recording.id, status="finalizing", stop_reason=reason)
        self._spawn_finalize(recording.id)
        self._status_dirty.set()
        return self.recording(recording.id)

    def _spawn_finalize(self, recording_id: str) -> None:
        task = asyncio.create_task(self._finalize(recording_id), name=f"finalize-{recording_id}")
        self._finalizing.add(task)
        task.add_done_callback(self._finalizing.discard)

    async def _finalize(self, recording_id: str) -> None:
        try:
            summary = await asyncio.to_thread(
                finalize_capture, self.recordings_dir / recording_id, self.historical
            )
            self.registry.update_live_recording(
                recording_id,
                status="completed",
                finished_at=now(),
                stop_reason=summary["stop_reason"],
                summary_json=summary,
            )
        except Exception as error:
            self.registry.update_live_recording(
                recording_id,
                status="failed",
                finished_at=now(),
                error_code=type(error).__name__,
                error_message="the capture could not be finalized; the capture file is kept",
            )
        self._status_dirty.set()

    def recording(self, recording_id: str) -> Json:
        row = (
            self.registry.live_recording(recording_id)
            if RECORDING_ID.fullmatch(recording_id)
            else None
        )
        if row is None:
            raise ServiceError(404, "RecordingNotFound", "live recording ID is unknown")
        return self._recording_view(row)

    def recordings(self, limit: int) -> Json:
        rows = self.registry.live_recordings(limit)
        return {"recordings": [self._recording_view(row) for row in rows], "count": len(rows)}

    def _recording_view(self, row: Row) -> Json:
        active = self._recording
        live = active.writer.stats() if active is not None and active.id == row["id"] else None
        return {**row, "progress": live, "disclaimer": DISCLAIMER}

    # ---- status ---------------------------------------------------------------------------

    def _feed_status(self) -> Json:
        gateway = self._gateway
        configured = bool(self.settings.alpaca_key_id and self.settings.alpaca_secret_key)
        if gateway is not None:
            return {
                **gateway.status(),
                "credentials_configured": configured or gateway.endpoint.feed == "mock",
                "transitions": list(self._feed_events),
            }
        endpoint = resolve_endpoint(self._feed, self.settings.live_mock_url or "ws://127.0.0.1:1")
        return {
            "state": FeedState.STOPPED.value,
            "feed": endpoint.feed,
            "provider": endpoint.provider,
            "source_label": endpoint.source_label,
            "test_data": endpoint.test_data,
            "scope": endpoint.scope,
            "endpoint_host": None,
            "desired_symbols": sorted(self._watchlists[self._feed]),
            "subscribed": {"trades": [], "quotes": []},
            "subscription_status": "none",
            "impairments": [],
            "reconnects": 0,
            "connections": 0,
            "attempt": 0,
            "next_retry_in_ms": None,
            "connected_since": None,
            "last_reconnect_at": None,
            "last_message_at": None,
            "last_message_age_ms": None,
            "frames": 0,
            "data_frames": 0,
            "messages": 0,
            "protocol_errors": 0,
            "last_error": self._start_error,
            "recent_errors": [self._start_error] if self._start_error else [],
            "credentials_configured": configured or endpoint.feed == "mock",
            "transitions": list(self._feed_events),
        }

    def _market_status(self, feed: Json) -> Json:
        age_ms = (
            None
            if self._last_observation_monotonic is None
            else round((time.monotonic() - self._last_observation_monotonic) * 1000)
        )
        if feed["state"] not in (FeedState.CONNECTED.value, FeedState.DEGRADED.value):
            activity = "offline"
        elif age_ms is None:
            activity = "none_yet"
        elif age_ms <= self.settings.live_stale_after_seconds * 1000:
            activity = "active"
        else:
            activity = "quiet"
        return {
            # Connection state says the transport is up. Only this says data is arriving.
            "activity": activity,
            "last_observation_at": None
            if self._last_observation_wall_ns is None
            else iso(self._last_observation_wall_ns),
            "last_observation_age_ms": age_ms,
            "stale_after_ms": round(self.settings.live_stale_after_seconds * 1000),
            **session_hint(feed["feed"]),
        }

    def _pipeline_status(self) -> Json:
        latency = sorted(self._latency_ns)
        native = sorted(self._native_ns_per_message)
        counters = (
            self._normalizer.snapshot(time.time_ns(), time.monotonic_ns())["counters"]
            if self._normalizer is not None
            else {}
        )
        return {
            "inbox_depth": len(self._inbox),
            "inbox_capacity": self.settings.live_inbox_frames,
            "inbox_high_water": self._counts["inbox_high_water"],
            "dropped_frames": self._counts["dropped_frames"],
            "dropped_messages": self._counts["dropped_messages"],
            "batches": self._counts["batches"],
            "frames": self._counts["frames"],
            "messages": self._counts["messages"],
            "events": self._counts["events"],
            "rejected": self._counts["rejected"],
            "duplicates": counters.get("duplicates", 0),
            "out_of_order": counters.get("out_of_order", 0),
            "unsupported": counters.get("unsupported", {}),
            "recent_rejects": list(self._rejects)[-10:],
            **self._rates,
            "flush_interval_ms": round(self.settings.live_flush_interval_seconds * 1000, 3),
            # Gateway receipt to fan-out enqueue, on this process's monotonic clock. It
            # includes the coalescing window and excludes the network on both sides.
            "local_latency_us": {
                "p50": _micros(_percentile(latency, 0.5)),
                "p99": _micros(_percentile(latency, 0.99)),
                "max": _micros(latency[-1] if latency else None),
                "samples": len(latency),
            },
            "native_ns_per_message": {
                "p50": _percentile(native, 0.5),
                "p99": _percentile(native, 0.99),
                "samples": len(native),
            },
        }

    def _clients_status(self) -> Json:
        return {
            "connected": len(self._clients),
            "capacity": self.settings.live_max_clients,
            "queue_capacity": self.settings.live_client_queue_messages,
            "queue_depth_max": max((len(client.queue) for client in self._clients), default=0),
            "queue_high_water": self._counts["client_queue_high_water"],
            "dropped_messages": self._counts["client_dropped_messages"],
            "dropped_events": self._counts["client_dropped_events"],
            "overflows": self._counts["client_overflows"],
            "slow_disconnects": self._counts["slow_disconnects"],
            "refused": self._counts["refused_clients"],
        }

    def _recording_status(self) -> Json:
        active = self._recording
        latest = self.registry.live_recordings(5)
        return {
            "active": active is not None,
            "id": active.id if active else None,
            "started_at": active.started_at if active else None,
            "upstream": None if active is None else ("up" if active.upstream_up else "down"),
            "gaps": active.gaps if active else 0,
            "progress": active.writer.stats() if active else None,
            "finalizing": [row["id"] for row in latest if row["status"] == "finalizing"],
            "latest": [
                {
                    "id": row["id"],
                    "feed": row["feed"],
                    "status": row["status"],
                    "created_at": row["created_at"],
                    "finished_at": row["finished_at"],
                    "stop_reason": row["stop_reason"],
                    "error_code": row["error_code"],
                    "sessions": (row["summary"] or {}).get("symbols", {}),
                    "coverage_gaps": len((row["summary"] or {}).get("coverage_gaps", [])),
                    "accepted_records": (row["summary"] or {}).get("accepted_records"),
                }
                for row in latest
            ],
        }

    def _status_payload(self) -> Json:
        feed = self._feed_status()
        return {
            "status": {
                "feed": feed,
                "market": self._market_status(feed),
                "pipeline": self._pipeline_status(),
                "clients": self._clients_status(),
                "recording": self._recording_status(),
                "clock": {
                    "server_wall_ns": str(time.time_ns()),
                    "note": "ages are intervals on the service's monotonic clock; comparing "
                    "source timestamps with this wall clock includes unknown clock offset",
                },
            },
            "states": self._states,
        }

    def status(self) -> Json:
        self._refresh_states()
        payload = self._status_payload()
        return {
            "protocol": PROTOCOL_VERSION,
            "stream": self._stream,
            "seq": self._seq,
            **payload["status"],
        }

    def health(self) -> Json:
        feed = self._feed_status()
        state = feed["state"]
        if state == FeedState.STOPPED.value:
            verdict = "failed" if feed["last_error"] else "stopped"
        elif state == FeedState.CONNECTED.value:
            verdict = "ok"
        elif state in (FeedState.DEGRADED.value, FeedState.RECONNECTING.value):
            verdict = "degraded"
        else:
            verdict = "connecting"
        return {
            "status": verdict,
            "feed_state": state,
            "feed": feed["feed"],
            "source_label": feed["source_label"],
            "market_activity": self._market_status(feed)["activity"],
            "last_error": feed["last_error"],
            "dropped_messages": self._counts["dropped_messages"],
            "client_dropped_messages": self._counts["client_dropped_messages"],
            "recording_active": self._recording is not None,
            "clients": len(self._clients),
        }
