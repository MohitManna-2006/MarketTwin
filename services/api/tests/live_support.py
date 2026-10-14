"""A mock Alpaca stream on its own thread, driven synchronously from tests."""

from __future__ import annotations

import asyncio
import threading
import time
from collections.abc import Callable, Coroutine
from typing import Any

from markettwin_api.live_mock import MockAlpacaStream, timestamp

Json = dict[str, Any]


class ThreadedMock:
    def __init__(self, **options: Any) -> None:
        self.loop = asyncio.new_event_loop()
        self.thread = threading.Thread(target=self.loop.run_forever, daemon=True)
        self.thread.start()
        self.stream = self.call(self._create(options))
        self.call(self.stream.start())

    @staticmethod
    async def _create(options: dict[str, Any]) -> MockAlpacaStream:
        return MockAlpacaStream(**options)

    def call(self, work: Coroutine[Any, Any, Any]) -> Any:
        return asyncio.run_coroutine_threadsafe(work, self.loop).result(10)

    @property
    def url(self) -> str:
        return self.stream.url

    def emit(self, messages: list[Json]) -> None:
        self.call(self.stream.emit(messages))

    def drop(self, refuse_seconds: float = 0.0) -> None:
        # Not awaited: the close handshake needs the peer, which may be this very thread.
        asyncio.run_coroutine_threadsafe(self.stream.drop(refuse_seconds), self.loop)

    def close(self) -> None:
        self.call(self.stream.stop())
        self.loop.call_soon_threadsafe(self.loop.stop)
        self.thread.join(5)


def trade(symbol: str, price: float, size: int, identifier: int, at: str | None = None) -> Json:
    return {
        "T": "t",
        "S": symbol,
        "i": identifier,
        "x": "V",
        "p": price,
        "s": size,
        "c": ["@"],
        "z": "C",
        "t": at or timestamp(),
    }


def quote(symbol: str, bid: float, ask: float, at: str | None = None) -> Json:
    return {
        "T": "q",
        "S": symbol,
        "bx": "V",
        "bp": bid,
        "bs": 2,
        "ax": "V",
        "ap": ask,
        "as": 3,
        "c": ["R"],
        "z": "C",
        "t": at or timestamp(),
    }


def until(condition: Callable[[], Any], timeout: float = 10.0, message: str = "condition") -> Any:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = condition()
        if value:
            return value
        time.sleep(0.01)
    raise AssertionError(f"{message} was not reached within {timeout} s")


async def eventually(
    condition: Callable[[], Any], timeout: float = 10.0, message: str = "condition"
) -> Any:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = condition()
        if value:
            return value
        await asyncio.sleep(0.01)
    raise AssertionError(f"{message} was not reached within {timeout} s")
