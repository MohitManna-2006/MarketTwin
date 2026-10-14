"""A local stand-in for Alpaca's stock stream, for tests and offline demonstrations.

``python -m markettwin_api.live_mock --port 8765`` serves the provider's documented
protocol on the loopback interface: the connected greeting, message authentication,
subscribe and unsubscribe with acknowledgments, numbered error frames, a connection limit,
and a symbol limit. Everything it emits is FABRICATED. The hub labels the ``mock`` feed
that way on every screen and in every recording, and never sends real credentials to it.

Paths: ``/v2/<anything>`` is the stream, ``/control`` takes JSON commands (``outage``,
``pause``, ``resume``, ``emit``) so a test can break and restore the upstream, and a plain
HTTP request to ``/health`` answers 200 for process supervisors.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import http
import json
import time
import zlib
from datetime import UTC, datetime
from typing import Any

from websockets.asyncio.server import Server, ServerConnection, serve
from websockets.exceptions import ConnectionClosed
from websockets.http11 import Request, Response

Json = dict[str, Any]


def timestamp(wall_ns: int | None = None) -> str:
    """RFC3339 UTC with nine fractional digits, as the provider writes it."""
    wall = time.time_ns() if wall_ns is None else wall_ns
    seconds, fraction = divmod(wall, 1_000_000_000)
    return datetime.fromtimestamp(seconds, UTC).strftime("%Y-%m-%dT%H:%M:%S") + f".{fraction:09d}Z"


class MockAlpacaStream:
    def __init__(
        self,
        *,
        host: str = "127.0.0.1",
        port: int = 0,
        rate: float = 0.0,
        key: str = "mock-key",
        secret: str = "mock-secret",
        connection_limit: int = 1,
        symbol_limit: int = 30,
    ) -> None:
        self.host = host
        self.port = port
        self.rate = rate
        self.key = key
        self.secret = secret
        self.connection_limit = connection_limit
        self.symbol_limit = symbol_limit
        self.sessions: dict[ServerConnection, Json] = {}
        self.auth_attempts = 0
        self.accepted = 0
        self.received: list[Json] = []
        self.paused = False
        self.refuse_until = 0.0
        # Set to a provider error code to refuse the next authentication with it.
        self.fail_auth_with: int | None = None
        self.silent = False
        self._server: Server | None = None
        self._ticker: asyncio.Task[None] | None = None
        self._ticks: dict[str, int] = {}
        self._trade_id = 0

    @property
    def url(self) -> str:
        return f"ws://{self.host}:{self.port}/v2/mock"

    async def start(self) -> None:
        self._server = await serve(
            self._handle, self.host, self.port, process_request=self._http, max_size=1 << 20
        )
        self.port = next(iter(self._server.sockets)).getsockname()[1]
        if self.rate > 0:
            self._ticker = asyncio.create_task(self._tick())

    async def stop(self) -> None:
        if self._ticker is not None:
            self._ticker.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self._ticker
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()

    # ---- test controls --------------------------------------------------------------------

    async def drop(self, refuse_seconds: float = 0.0) -> None:
        """Close every stream connection, optionally refusing new ones for a while."""
        self.refuse_until = time.monotonic() + refuse_seconds
        for connection in list(self.sessions):
            await connection.close(1011, "mock outage")

    async def emit(self, messages: list[Json]) -> None:
        """Send one data frame to every session, filtered by its subscription."""
        for connection, session in list(self.sessions.items()):
            selected = [
                message
                for message in messages
                if message.get("S")
                in session["trades" if message.get("T") in ("t", "c", "x") else "quotes"]
            ]
            if selected:
                with contextlib.suppress(ConnectionClosed):
                    await connection.send(json.dumps(selected, separators=(",", ":")))

    async def emit_raw(self, text: str) -> None:
        for connection in list(self.sessions):
            with contextlib.suppress(ConnectionClosed):
                await connection.send(text)

    def fabricate(self, symbol: str) -> list[Json]:
        """One quote and one trade on a deterministic walk. Prices are whole cents."""
        tick = self._ticks.get(symbol, 0)
        self._ticks[symbol] = tick + 1
        base = 5_000 + (zlib.crc32(symbol.encode()) % 40_000)
        # A slow triangle wave with a small deterministic wobble: a walk, not a market.
        phase = tick % 240
        offset = (phase if phase < 120 else 240 - phase) // 4 + (
            zlib.crc32(bytes([tick % 251])) % 3
        )
        bid = base + offset
        self._trade_id += 1
        return [
            {
                "T": "q",
                "S": symbol,
                "bx": "V",
                "bp": bid / 100,
                "bs": 1 + tick % 5,
                "ax": "V",
                "ap": (bid + 2) / 100,
                "as": 1 + (tick + 2) % 5,
                "c": ["R"],
                "z": "C",
                "t": timestamp(),
            },
            {
                "T": "t",
                "S": symbol,
                "i": self._trade_id,
                "x": "V",
                "p": (bid + 1) / 100,
                "s": 10 * (1 + tick % 7),
                "c": ["@"],
                "z": "C",
                "t": timestamp(),
            },
        ]

    async def _tick(self) -> None:
        while True:
            await asyncio.sleep(1 / self.rate)
            if self.paused:
                continue
            symbols = {symbol for session in self.sessions.values() for symbol in session["trades"]}
            for symbol in sorted(symbols):
                await self.emit(self.fabricate(symbol))

    # ---- protocol -------------------------------------------------------------------------

    def _http(self, connection: ServerConnection, request: Request) -> Response | None:
        if request.path == "/health":
            return connection.respond(http.HTTPStatus.OK, "ok\n")
        return None

    async def _handle(self, connection: ServerConnection) -> None:
        path = connection.request.path if connection.request else ""
        if path == "/control":
            await self._control(connection)
            return
        if time.monotonic() < self.refuse_until:
            await connection.close(1013, "mock outage")
            return
        self.accepted += 1
        if self.silent:
            await connection.wait_closed()
            return
        await connection.send('[{"T":"success","msg":"connected"}]')
        session: Json = {"authenticated": False, "trades": [], "quotes": []}
        try:
            async for raw in connection:
                try:
                    message = json.loads(raw)
                except ValueError:
                    await self._error(connection, 400, "invalid syntax")
                    continue
                self.received.append(
                    {key: value for key, value in message.items() if key != "secret"}
                )
                action = message.get("action")
                if action == "auth":
                    await self._auth(connection, session, message)
                elif action in ("subscribe", "unsubscribe"):
                    await self._subscribe(connection, session, message)
                else:
                    await self._error(connection, 400, "invalid syntax")
        except ConnectionClosed:
            pass
        finally:
            self.sessions.pop(connection, None)

    async def _error(self, connection: ServerConnection, code: int, message: str) -> None:
        await connection.send(json.dumps([{"T": "error", "code": code, "msg": message}]))

    async def _auth(self, connection: ServerConnection, session: Json, message: Json) -> None:
        self.auth_attempts += 1
        if session["authenticated"]:
            await self._error(connection, 403, "already authenticated")
        elif self.fail_auth_with is not None:
            code, self.fail_auth_with = self.fail_auth_with, None
            text = {409: "insufficient subscription", 500: "internal error"}.get(
                code, "auth failed"
            )
            await self._error(connection, code, text)
        elif message.get("key") != self.key or message.get("secret") != self.secret:
            await self._error(connection, 402, "auth failed")
        elif len(self.sessions) >= self.connection_limit:
            await self._error(connection, 406, "connection limit exceeded")
        else:
            session["authenticated"] = True
            self.sessions[connection] = session
            await connection.send('[{"T":"success","msg":"authenticated"}]')

    async def _subscribe(self, connection: ServerConnection, session: Json, message: Json) -> None:
        if not session["authenticated"]:
            await self._error(connection, 401, "not authenticated")
            return
        updated = {channel: set(session[channel]) for channel in ("trades", "quotes")}
        for channel in ("trades", "quotes"):
            symbols = message.get(channel) or []
            if message["action"] == "subscribe":
                updated[channel] |= set(symbols)
            else:
                updated[channel] -= set(symbols)
        if len(updated["trades"] | updated["quotes"]) > self.symbol_limit:
            # The provider leaves the subscription unchanged and sends no acknowledgment.
            await self._error(connection, 405, "symbol limit exceeded")
            return
        for channel in ("trades", "quotes"):
            session[channel] = sorted(updated[channel])
        await connection.send(
            json.dumps(
                [
                    {
                        "T": "subscription",
                        "trades": session["trades"],
                        "quotes": session["quotes"],
                        "bars": [],
                        "updatedBars": [],
                        "dailyBars": [],
                        "statuses": [],
                        "lulds": [],
                        "corrections": session["trades"],
                        "cancelErrors": session["trades"],
                    }
                ]
            )
        )

    async def _control(self, connection: ServerConnection) -> None:
        with contextlib.suppress(ConnectionClosed):
            async for raw in connection:
                command = json.loads(raw)
                action = command.get("action")
                if action == "outage":
                    await self.drop(float(command.get("seconds", 0)))
                elif action == "pause":
                    self.paused = True
                elif action == "resume":
                    self.paused = False
                elif action == "emit":
                    await self.emit(command.get("messages", []))
                await connection.send(json.dumps({"ok": True, "sessions": len(self.sessions)}))


async def _main(arguments: argparse.Namespace) -> None:
    stream = MockAlpacaStream(port=arguments.port, rate=arguments.rate)
    await stream.start()
    print(f"mock Alpaca stream (FABRICATED DATA) at {stream.url}", flush=True)
    await asyncio.Event().wait()


def main() -> None:
    parser = argparse.ArgumentParser(description="Mock Alpaca stock stream on the loopback")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--rate", type=float, default=5.0, help="frames per second per symbol")
    with contextlib.suppress(KeyboardInterrupt):
        asyncio.run(_main(parser.parse_args()))


if __name__ == "__main__":
    main()
