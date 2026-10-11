"""Acquisition only. The native core validates and normalizes every market number."""

from __future__ import annotations

import hashlib
import json
import shutil
import time
from dataclasses import dataclass
from decimal import Decimal
from pathlib import Path
from typing import Protocol

import httpx

from markettwin_api.registry import now
from markettwin_api.service import ServiceError
from markettwin_api.settings import Settings


@dataclass(frozen=True)
class Acquisition:
    input_path: Path
    metadata: dict[str, object]


class HistoricalDataProvider(Protocol):
    def acquire(
        self,
        symbol: str,
        feed: str,
        start: str | None,
        end: str | None,
        raw_dir: Path,
    ) -> Acquisition: ...


class LocalFileProvider:
    """Only named, bundled fabricated fixtures. No request-supplied path."""

    def __init__(self, sources: Path) -> None:
        self.sources = sources

    def datasets(self) -> list[dict[str, str]]:
        return [
            {
                "id": "aapl_fixture",
                "symbol": "AAPL",
                "feed": "fabricated_l1",
                "description": "Fabricated L1 test observations; not real market data",
            }
        ]

    def acquire(
        self,
        symbol: str,
        feed: str,
        start: str | None,
        end: str | None,
        raw_dir: Path,
    ) -> Acquisition:
        if symbol != "AAPL" or feed != "fabricated_l1" or start or end:
            raise ServiceError(
                422, "UnknownLocalDataset", "select the bundled AAPL fabricated fixture"
            )
        source = self.sources / "aapl_records.jsonl"
        if not source.is_file():
            raise ServiceError(503, "FixtureUnavailable", "offline fixture is not installed")
        raw_dir.mkdir(parents=True, exist_ok=False)
        retained = raw_dir / "records.jsonl"
        shutil.copyfile(source, retained)
        return Acquisition(
            retained,
            {
                "symbol": symbol,
                "provider": "local_file",
                "feed": feed,
                "scope": "single_venue",
                "license": "fabricated_test_fixture",
                "acquired_at": now(),
                "raw_source_sha256": hashlib.sha256(retained.read_bytes()).hexdigest(),
            },
        )


class AlpacaProvider:
    """Historical stock trades/quotes, explicitly selected IEX or SIP feed."""

    BASE_URL = "https://data.alpaca.markets/v2/stocks"

    def __init__(self, settings: Settings) -> None:
        self.settings = settings

    def acquire(
        self,
        symbol: str,
        feed: str,
        start: str | None,
        end: str | None,
        raw_dir: Path,
    ) -> Acquisition:
        if not self.settings.alpaca_key_id or not self.settings.alpaca_secret_key:
            raise ServiceError(503, "ProviderNotConfigured", "Alpaca data credentials are missing")
        if feed not in ("iex", "sip") or not start or not end:
            raise ServiceError(422, "InvalidProviderRequest", "select IEX or SIP and a UTC range")
        raw_dir.mkdir(parents=True, exist_ok=False)
        staging = raw_dir / "records.jsonl"
        page_hashes: dict[str, str] = {}
        with httpx.Client(timeout=30) as client, staging.open("w", encoding="utf-8") as output:
            for kind in ("trades", "quotes"):
                page_token: str | None = None
                seen_tokens: set[str] = set()
                for page in range(self.settings.historical_max_pages):
                    params: dict[str, str | int] = {
                        "start": start,
                        "end": end,
                        "feed": feed,
                        "sort": "asc",
                        "limit": 10000,
                    }
                    if page_token:
                        params["page_token"] = page_token
                    response = self._get(client, f"{self.BASE_URL}/{symbol}/{kind}", params)
                    page_name = f"{kind}-{page + 1:03d}.json"
                    (raw_dir / page_name).write_bytes(response.content)
                    page_hashes[page_name] = hashlib.sha256(response.content).hexdigest()
                    try:
                        body = json.loads(response.content, parse_float=Decimal)
                        records = body[kind]
                        if not isinstance(records, list):
                            raise ValueError("provider records are not a list")
                        for record in records:
                            if not isinstance(record, dict):
                                raise ValueError("provider record is not an object")
                            converted = {
                                key: value
                                for key, value in record.items()
                                if key
                                in (
                                    "t",
                                    "p",
                                    "s",
                                    "i",
                                    "x",
                                    "c",
                                    "bp",
                                    "ap",
                                    "bs",
                                    "as",
                                    "bx",
                                    "ax",
                                    "z",
                                )
                            }
                            for price in ("p", "bp", "ap"):
                                if price in converted:
                                    converted[price] = str(converted[price])
                            converted["kind"] = "trade" if kind == "trades" else "quote"
                            converted["symbol"] = symbol
                            output.write(json.dumps(converted, separators=(",", ":")) + "\n")
                        page_token = body.get("next_page_token")
                    except (KeyError, TypeError, ValueError) as error:
                        raise ServiceError(
                            502, "ProviderResponseInvalid", "Alpaca returned invalid market data"
                        ) from error
                    if not page_token:
                        break
                    if page_token in seen_tokens:
                        raise ServiceError(
                            502, "ProviderPaginationLoop", "Alpaca repeated a page token"
                        )
                    seen_tokens.add(page_token)
                else:
                    raise ServiceError(
                        413,
                        "ProviderPageLimit",
                        "historical range exceeds the configured page limit",
                    )
        return Acquisition(
            staging,
            {
                "symbol": symbol,
                "provider": "alpaca",
                "feed": feed,
                "scope": "single_venue" if feed == "iex" else "consolidated",
                "license": "alpaca_terms_apply_no_redistribution",
                "acquired_at": now(),
                "requested_start": start,
                "requested_end": end,
                "raw_page_sha256": page_hashes,
            },
        )

    def _get(self, client: httpx.Client, url: str, params: dict[str, str | int]) -> httpx.Response:
        for attempt in range(3):
            try:
                response = client.get(
                    url,
                    params=params,
                    headers={
                        "APCA-API-KEY-ID": self.settings.alpaca_key_id,
                        "APCA-API-SECRET-KEY": self.settings.alpaca_secret_key,
                    },
                )
            except (httpx.TimeoutException, httpx.ConnectError) as error:
                if attempt == 2:
                    raise ServiceError(
                        503, "ProviderUnavailable", "Alpaca data API is unavailable"
                    ) from error
                time.sleep(0.25 * (2**attempt))
                continue
            if response.status_code in (429, 500, 502, 503, 504) and attempt < 2:
                time.sleep(min(2.0, 0.25 * (2**attempt)))
                continue
            if response.status_code == 401:
                raise ServiceError(
                    401, "ProviderAuthentication", "Alpaca rejected data credentials"
                )
            if response.status_code == 403:
                raise ServiceError(403, "ProviderEntitlement", "Alpaca feed access is not entitled")
            if response.status_code >= 400:
                raise ServiceError(502, "ProviderFailure", "Alpaca historical request failed")
            return response
        raise ServiceError(503, "ProviderUnavailable", "Alpaca retry budget exhausted")
