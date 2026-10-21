"""Calibration specs, candidate configs, and session windows (ADR 0011).

A spec fixes everything a calibration decides before it sees data: the symbol, the
window, the split, the parameter grid, the search sizes, and the seed ranges. Nothing here
computes a market statistic or a score; the native core does both.
"""

from __future__ import annotations

import copy
import hashlib
import itertools
import json
from datetime import date, datetime, time
from typing import Any, Literal
from zoneinfo import ZoneInfo

from pydantic import Field, field_validator

from markettwin_api.config_models import Strict

Json = dict[str, Any]

GRID_KEYS = (
    "noise_interval_ns",
    "noise_market_order_ppm",
    "noise_max_quantity",
    "noise_limit_offset_ticks",
    "maker_interval_ns",
    "maker_half_spread_ticks",
    "maker_quote_size",
    "maker_level_spacing_ticks",
    "fundamental_volatility_milliticks",
)
MAKER_POLICIES = ("MarketMaker", "InventoryMarketMaker")
ROLES = ("train", "validation", "test")
# ADR 0011: at least this many admitted sessions per role, or the calibration fails.
MINIMUM_ADMITTED = {"train": 4, "validation": 1, "test": 1}
# ADR 0011 acceptance: |d| <= 2 on at least 9 of the 12 facts, and beat the baseline.
ACCEPT_DISTANCE_PPM = 2_000_000
ACCEPT_FACTS = 9
GATE_TWO_SIDED_PPM = 900_000


class WindowSpec(Strict):
    start_local: str = Field(pattern=r"^([01]\d|2[0-3]):[0-5]\d$")
    end_local: str = Field(pattern=r"^([01]\d|2[0-3]):[0-5]\d$")
    timezone: Literal["America/New_York"]


class SplitSpec(Strict):
    train: int = Field(ge=4, le=20)
    validation: int = Field(ge=1, le=20)
    test: int = Field(ge=1, le=20)


class SearchSpec(Strict):
    seed: int = Field(ge=0, le=2**63 - 1)
    stage1_candidates: int = Field(ge=1, le=4096)
    stage1_seeds: int = Field(ge=2, le=100)
    stage2_top: int = Field(ge=1, le=64)
    stage2_seeds: int = Field(ge=2, le=500)
    test_seeds: int = Field(ge=2, le=1000)
    stage1_seed_base: int = Field(ge=0, le=2**62)
    stage2_seed_base: int = Field(ge=0, le=2**62)
    test_seed_base: int = Field(ge=0, le=2**62)


class GridSpec(Strict):
    noise_interval_ns: list[int] = Field(min_length=1, max_length=16)
    noise_market_order_ppm: list[int] = Field(min_length=1, max_length=16)
    noise_max_quantity: list[int] = Field(min_length=1, max_length=16)
    noise_limit_offset_ticks: list[int] = Field(min_length=1, max_length=16)
    maker_interval_ns: list[int] = Field(min_length=1, max_length=16)
    maker_half_spread_ticks: list[int] = Field(min_length=1, max_length=16)
    maker_quote_size: list[int] = Field(min_length=1, max_length=16)
    maker_level_spacing_ticks: list[int] = Field(min_length=1, max_length=16)
    fundamental_volatility_milliticks: list[int] = Field(min_length=1, max_length=16)

    @field_validator("*")
    @classmethod
    def _distinct_positive(cls, values: list[int]) -> list[int]:
        if any(value <= 0 for value in values) or len(set(values)) != len(values):
            raise ValueError("grid values must be distinct positive integers")
        return values


class CalibrationSpec(Strict):
    schema_version: Literal[1]
    title: str = Field(min_length=1, max_length=120)
    symbol: str = Field(pattern=r"^[A-Z][A-Z.]{0,11}$")
    feed: Literal["iex", "fabricated_l1"]
    window: WindowSpec
    base_config: str = Field(pattern=r"^[a-z0-9_]+\.json$")
    split: SplitSpec
    search: SearchSpec
    grid: GridSpec

    def seeds(self, stage: str) -> list[int]:
        base, count = {
            "stage1": (self.search.stage1_seed_base, self.search.stage1_seeds),
            "stage2": (self.search.stage2_seed_base, self.search.stage2_seeds),
            "test": (self.search.test_seed_base, self.search.test_seeds),
        }[stage]
        return list(range(base, base + count))

    @property
    def session_count(self) -> int:
        return self.split.train + self.split.validation + self.split.test

    @property
    def grid_size(self) -> int:
        size = 1
        for key in GRID_KEYS:
            size *= len(getattr(self.grid, key))
        return size


def spec_digest(spec: CalibrationSpec) -> str:
    return hashlib.sha256(canonical_json(spec.model_dump())).hexdigest()


def canonical_json(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def problems(spec: CalibrationSpec) -> list[str]:
    """Reasons the spec cannot run at all."""
    found: list[str] = []
    ranges = sorted(
        (base, base + count, name)
        for name, base, count in (
            ("stage 1", spec.search.stage1_seed_base, spec.search.stage1_seeds),
            ("stage 2", spec.search.stage2_seed_base, spec.search.stage2_seeds),
            ("test", spec.search.test_seed_base, spec.search.test_seeds),
        )
    )
    for (_, end, first), (start, _, second) in itertools.pairwise(ranges):
        if start < end:
            found.append(f"the {first} and {second} seed ranges overlap")
    if spec.search.stage1_candidates > spec.grid_size:
        found.append("the grid has fewer points than stage 1 draws candidates")
    if spec.search.stage2_top > spec.search.stage1_candidates + 1:
        found.append("stage 2 keeps more candidates than stage 1 scores")
    if _minutes(spec.window.end_local) <= _minutes(spec.window.start_local):
        found.append("the window must end after it starts")
    return found


def deviations(spec: CalibrationSpec) -> list[str]:
    """Where the spec departs from ADR 0011. Observed IEX data must not depart at all."""
    found: list[str] = []
    if (spec.window.start_local, spec.window.end_local) != ("10:30", "11:30"):
        found.append("the window is not 10:30 to 11:30 America/New_York")
    if (spec.split.train, spec.split.validation, spec.split.test) != (6, 2, 2):
        found.append("the split is not 6 train, 2 validation, 2 test sessions")
    expected = {
        "stage1_candidates": 256,
        "stage1_seeds": 10,
        "stage2_top": 10,
        "stage2_seeds": 50,
        "test_seeds": 100,
    }
    for key, value in expected.items():
        if getattr(spec.search, key) != value:
            found.append(f"{key} is {getattr(spec.search, key)}, not {value}")
    return found


def _minutes(clock: str) -> int:
    hours, minutes = clock.split(":")
    return int(hours) * 60 + int(minutes)


def window_ns(spec: CalibrationSpec) -> int:
    return (_minutes(spec.window.end_local) - _minutes(spec.window.start_local)) * 60 * 10**9


def session_window(spec: CalibrationSpec, day: date) -> tuple[datetime, datetime]:
    """The window on one local trading day, as UTC instants."""
    zone = ZoneInfo(spec.window.timezone)
    start = datetime.combine(day, time.fromisoformat(spec.window.start_local), zone)
    end = datetime.combine(day, time.fromisoformat(spec.window.end_local), zone)
    return start.astimezone(ZoneInfo("UTC")), end.astimezone(ZoneInfo("UTC"))


def utc_text(instant: datetime) -> str:
    return instant.astimezone(ZoneInfo("UTC")).isoformat().replace("+00:00", "Z")


def utc_ns(instant: datetime) -> int:
    epoch = datetime(1970, 1, 1, tzinfo=ZoneInfo("UTC"))
    delta = instant - epoch
    return (delta.days * 86_400 + delta.seconds) * 10**9 + delta.microseconds * 1000


def parse_utc(text: str) -> datetime:
    parsed = datetime.fromisoformat(text.replace("Z", "+00:00"))
    if parsed.tzinfo is None:
        raise ValueError("timestamp has no offset")
    return parsed


def local_day(spec: CalibrationSpec, start: datetime) -> date:
    return start.astimezone(ZoneInfo(spec.window.timezone)).date()


# ---- candidates -----------------------------------------------------------------------


def draw_candidates(spec: CalibrationSpec) -> list[Json]:
    """Stage 1 draws: uniform over the grid without replacement, from the search seed.

    Each coordinate is a SHA-256 of (seed, candidate, attempt, parameter), so the draw is
    the same on every platform and Python version.
    """
    seen: set[tuple[int, ...]] = set()
    drawn: list[Json] = []
    for index in range(1, spec.search.stage1_candidates + 1):
        attempt = 0
        while True:
            point: Json = {}
            for key in GRID_KEYS:
                values = getattr(spec.grid, key)
                digest = hashlib.sha256(
                    f"{spec.search.seed}:{index}:{attempt}:{key}".encode()
                ).digest()
                point[key] = values[int.from_bytes(digest[:8], "big") % len(values)]
            identity = tuple(point[key] for key in GRID_KEYS)
            if identity not in seen:
                seen.add(identity)
                drawn.append(point)
                break
            attempt += 1
    return drawn


def candidate_config(
    base: Json,
    parameters: Json | None,
    reference_price_cents: int,
    seed: int,
    run_id: str,
) -> Json:
    """The run config of one candidate and seed. ``None`` keeps the base agents as they
    are (candidate 0, the baseline); only the starting price changes."""
    config = copy.deepcopy(base)
    config["seed"] = seed
    config["run_id"] = run_id
    scenario = config["scenario"]
    scenario["reference_price_ticks"] = reference_price_cents
    scenario["fundamental"]["initial_milliticks"] = reference_price_cents * 1000
    if parameters is None:
        return config
    scenario["fundamental"]["volatility_milliticks"] = parameters[
        "fundamental_volatility_milliticks"
    ]
    for agent in scenario["agents"]:
        policy = agent["policy"]
        if policy["kind"] == "Noise":
            agent["wakeup"]["interval_ns"] = parameters["noise_interval_ns"]
            policy["market_order_ppm"] = parameters["noise_market_order_ppm"]
            policy["max_quantity"] = parameters["noise_max_quantity"]
            policy["limit_offset_ticks"] = parameters["noise_limit_offset_ticks"]
        elif policy["kind"] in MAKER_POLICIES:
            agent["wakeup"]["interval_ns"] = parameters["maker_interval_ns"]
            policy["half_spread_ticks"] = parameters["maker_half_spread_ticks"]
            policy["quote_size"] = parameters["maker_quote_size"]
            policy["level_spacing_ticks"] = parameters["maker_level_spacing_ticks"]
    return config


def base_problems(spec: CalibrationSpec, base: Json) -> list[str]:
    """Why a base config cannot carry this spec's grid."""
    found: list[str] = []
    scenario = base.get("scenario")
    if not isinstance(scenario, dict):
        return ["the base config has no scenario"]
    if base.get("tick_size_minor") != 1:
        found.append("the base config's tick must be one cent (tick_size_minor 1)")
    if base.get("horizon_ns") != window_ns(spec):
        found.append("the base config's horizon must equal the window")
    if scenario.get("shocks"):
        found.append("the base config must have no shocks")
    agents = scenario.get("agents", [])
    noise = [agent for agent in agents if agent["policy"]["kind"] == "Noise"]
    makers = [agent for agent in agents if agent["policy"]["kind"] in MAKER_POLICIES]
    if not noise or not makers:
        found.append("the base config needs noise traders and makers")
    for agent in noise:
        if max(spec.grid.noise_max_quantity) > agent["limits"]["max_order_quantity"]:
            found.append(f"noise_max_quantity exceeds agent {agent['id']}'s order limit")
        if agent["policy"]["min_quantity"] > min(spec.grid.noise_max_quantity):
            found.append(f"noise_max_quantity is below agent {agent['id']}'s min_quantity")
    for agent in makers:
        if max(spec.grid.maker_quote_size) > agent["limits"]["max_order_quantity"]:
            found.append(f"maker_quote_size exceeds agent {agent['id']}'s order limit")
    return found
