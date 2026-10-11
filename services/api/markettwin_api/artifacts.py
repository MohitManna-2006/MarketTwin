"""Read-only access to the artifacts of one completed run.

The core's files are never modified. The first time a run is opened this module makes one
pass over its three streams and writes a derived, columnar copy to the workspace's
``derived`` directory: every number in it is a number the core recorded. Three series are
derived rather than recorded, and each is labelled so wherever it is served:

* ``mid_twice_ticks``: the recorded mark where its recorded source is ``Midpoint``.
* ``drawdown``: running peak of recorded equity minus recorded equity. The authoritative
  maximum is the core's ``max_drawdown_minor`` in metrics.json.
* ``cum_bought`` / ``cum_sold``: recorded fill quantities of ledger entries booked before
  each sample, in accounting-sequence order.

Lookups by time use the latest record at or before the requested time. Nothing is
interpolated.
"""

from __future__ import annotations

import json
import math
import threading
from array import array
from bisect import bisect_right
from collections import OrderedDict
from collections.abc import Callable, Iterable, Sequence
from pathlib import Path
from typing import Any

JS_SAFE_INTEGER = 2**53 - 1
INDEX_VERSION = 3
RUN_FILES = (
    "config.json",
    "commands.jsonl",
    "trace.jsonl",
    "accounting.jsonl",
    "metrics.json",
    "manifest.json",
)
AGENT_COLUMNS = (
    "cash_minor",
    "position",
    "cost_basis_minor",
    "realized_pnl_minor",
    "unrealized_pnl_minor",
    "equity_minor",
)
MARK_SOURCES = ("None", "Reference", "LastTrade", "Midpoint")
CHANNELS = ("commands", "execution")
_EVENT_COLUMNS = (
    ("channel", "B"),
    ("offset", "Q"),
    ("length", "I"),
    ("timestamp_ns", "Q"),
    ("sequence", "Q"),
    ("kind", "B"),
    ("actor", "q"),
    ("other", "q"),
    ("order", "q"),
)

Json = dict[str, Any]
Columns = dict[str, list[Any]]


class ArtifactError(Exception):
    """A run's artifacts cannot be served. ``code`` is stable; ``message`` says why."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code
        self.message = message


def _safe(value: Any) -> Any:
    if isinstance(value, int) and not isinstance(value, bool) and abs(value) > JS_SAFE_INTEGER:
        raise ArtifactError(
            "UnsupportedRange",
            f"a recorded value ({value}) exceeds 2^53 - 1 and cannot be shown exactly in a browser",
        )
    return value


# ---- binary column files -----------------------------------------------------------------


def _write_arrays(path: Path, columns: dict[str, array[Any]], extra: Json) -> None:
    header = {
        "columns": [[name, column.typecode, len(column)] for name, column in columns.items()],
        **extra,
    }
    with path.open("wb") as output:
        output.write(json.dumps(header).encode() + b"\n")
        for column in columns.values():
            output.write(column.tobytes())


def _read_arrays(path: Path) -> tuple[dict[str, array[Any]], Json]:
    with path.open("rb") as source:
        header = json.loads(source.readline())
        columns: dict[str, array[Any]] = {}
        for name, typecode, length in header["columns"]:
            column: array[Any] = array(typecode)
            column.frombytes(source.read(length * column.itemsize))
            columns[name] = column
    return columns, header


def _write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, separators=(",", ":")))


# ---- building the derived index ----------------------------------------------------------


class _EventIndex:
    def __init__(self) -> None:
        self.columns: dict[str, array[Any]] = {name: array(code) for name, code in _EVENT_COLUMNS}
        self.kinds: list[str] = []

    def add(self, channel: int, offset: int, length: int, record: Json) -> None:
        kind = record["kind"]
        if kind not in self.kinds:
            self.kinds.append(kind)
        payload = record.get("payload") or {}
        actor = record.get("actor_id")
        other = payload.get("maker_agent_id") if kind == "Trade" else None
        order = record.get("correlation_id")
        columns = self.columns
        columns["channel"].append(channel)
        columns["offset"].append(offset)
        columns["length"].append(length)
        columns["timestamp_ns"].append(_safe(record["timestamp_ns"]))
        columns["sequence"].append(_safe(record["sequence"]))
        columns["kind"].append(self.kinds.index(kind))
        columns["actor"].append(-1 if actor is None else _safe(actor))
        columns["other"].append(-1 if other is None else _safe(other))
        columns["order"].append(-1 if order is None else _safe(order))

    def sorted_by_sequence(self) -> dict[str, array[Any]]:
        order = sorted(
            range(len(self.columns["sequence"])), key=self.columns["sequence"].__getitem__
        )
        return {
            name: array(code, (self.columns[name][index] for index in order))
            for name, code in _EVENT_COLUMNS
        }


def _records(path: Path) -> Iterable[tuple[int, int, Json]]:
    """Every record after the header line, with its byte offset and length."""
    offset = 0
    with path.open("rb") as source:
        for number, line in enumerate(source):
            length = len(line)
            if number > 0:
                yield offset, length, json.loads(line)
            offset += length


def _header(path: Path) -> Json:
    with path.open("rb") as source:
        header: Json = json.loads(source.readline())
    return header


def build_index(run_dir: Path, cache_dir: Path, max_samples: int) -> None:
    """One pass over the run's streams. Raises ArtifactError and leaves no partial index."""
    manifest = json.loads((run_dir / "manifest.json").read_text())
    accounting_header = _header(run_dir / "accounting.jsonl")
    agent_ids: list[int] = [_safe(item) for item in accounting_header["agent_ids"]]
    if tuple(accounting_header["agent_columns"]) != AGENT_COLUMNS:
        raise ArtifactError("UnsupportedSchema", "accounting stream has unexpected agent columns")

    samples: Columns = {
        name: []
        for name in (
            "timestamp_ns",
            "sequence",
            "bid_price_ticks",
            "bid_quantity",
            "ask_price_ticks",
            "ask_quantity",
            "spread_ticks",
            "bid_depth",
            "ask_depth",
            "depth_levels",
            "mark_twice_ticks",
            "mark_source",
            "fundamental_milliticks",
            "trade_count",
            "volume",
        )
    }
    agents: dict[int, Columns] = {
        agent: {name: [] for name in (*AGENT_COLUMNS, "cum_bought", "cum_sold")}
        for agent in agent_ids
    }
    ledger: Columns = {
        name: []
        for name in (
            "timestamp_ns",
            "sequence",
            "trade_id",
            "price_ticks",
            "quantity",
            "notional_minor",
            "buyer",
            "buyer_cash_minor",
            "buyer_position",
            "buyer_cost_basis_minor",
            "buyer_realized_pnl_minor",
            "buyer_fee_minor",
            "seller",
            "seller_cash_minor",
            "seller_position",
            "seller_cost_basis_minor",
            "seller_realized_pnl_minor",
            "seller_fee_minor",
        )
    }
    accounting_index = _EventIndex()
    bought = dict.fromkeys(agent_ids, 0)
    sold = dict.fromkeys(agent_ids, 0)
    accounting_terminal: Json | None = None

    for offset, length, record in _records(run_dir / "accounting.jsonl"):
        accounting_index.add(0, offset, length, record)
        payload = record["payload"]
        kind = record["kind"]
        if kind == "Sample":
            if len(samples["timestamp_ns"]) >= max_samples:
                raise ArtifactError(
                    "UnsupportedRange",
                    f"the run recorded more than {max_samples:,} samples; "
                    "raise the sampling interval or shorten the horizon",
                )
            bid, ask = payload["bid"], payload["ask"]
            samples["timestamp_ns"].append(_safe(record["timestamp_ns"]))
            samples["sequence"].append(_safe(record["sequence"]))
            samples["bid_price_ticks"].append(_safe(bid[0]) if bid else None)
            samples["bid_quantity"].append(_safe(bid[1]) if bid else None)
            samples["ask_price_ticks"].append(_safe(ask[0]) if ask else None)
            samples["ask_quantity"].append(_safe(ask[1]) if ask else None)
            samples["spread_ticks"].append(_safe(payload["spread_ticks"]))
            samples["bid_depth"].append(_safe(payload["bid_depth"]))
            samples["ask_depth"].append(_safe(payload["ask_depth"]))
            samples["depth_levels"].append(payload["depth_levels"])
            samples["mark_twice_ticks"].append(_safe(payload["mark_twice_ticks"]))
            samples["mark_source"].append(MARK_SOURCES.index(payload["mark_source"]))
            samples["fundamental_milliticks"].append(_safe(payload["fundamental_milliticks"]))
            samples["trade_count"].append(_safe(payload["trade_count"]))
            samples["volume"].append(_safe(payload["volume"]))
            for agent, row in zip(agent_ids, payload["agents"], strict=True):
                columns = agents[agent]
                for name, value in zip(AGENT_COLUMNS, row, strict=True):
                    columns[name].append(_safe(value))
                columns["cum_bought"].append(bought[agent])
                columns["cum_sold"].append(sold[agent])
        elif kind == "LedgerEntry":
            buyer, seller = payload["buyer"], payload["seller"]
            quantity = _safe(payload["quantity"])
            bought[buyer["agent_id"]] += quantity
            sold[seller["agent_id"]] += quantity
            ledger["timestamp_ns"].append(_safe(record["timestamp_ns"]))
            ledger["sequence"].append(_safe(record["sequence"]))
            ledger["trade_id"].append(_safe(payload["trade_id"]))
            ledger["price_ticks"].append(_safe(payload["price_ticks"]))
            ledger["quantity"].append(quantity)
            ledger["notional_minor"].append(_safe(payload["notional_minor"]))
            for side, party in (("buyer", buyer), ("seller", seller)):
                ledger[side].append(party["agent_id"])
                for field in ("cash_minor", "position", "cost_basis_minor", "realized_pnl_minor"):
                    ledger[f"{side}_{field}"].append(_safe(party[field]))
                ledger[f"{side}_fee_minor"].append(_safe(party["fee_minor"]))
        elif kind == "AccountingCompleted":
            accounting_terminal = payload

    market_index = _EventIndex()
    books: Columns = {"timestamp_ns": [], "sequence": [], "bids": [], "asks": []}
    trades: Columns = {
        name: []
        for name in (
            "timestamp_ns",
            "sequence",
            "trade_id",
            "price_ticks",
            "quantity",
            "maker_agent_id",
            "taker_agent_id",
            "aggressor_side",
            "buy_order_id",
            "sell_order_id",
        )
    }
    shocks: list[Json] = []
    terminal: Json | None = None

    for offset, length, record in _records(run_dir / "trace.jsonl"):
        market_index.add(1, offset, length, record)
        kind = record["kind"]
        payload = record["payload"]
        if kind == "BookSnapshot":
            books["timestamp_ns"].append(record["timestamp_ns"])
            books["sequence"].append(record["sequence"])
            for side in ("bids", "asks"):
                books[side].append(
                    [
                        [
                            _safe(level["price_ticks"]),
                            _safe(level["quantity"]),
                            level["order_count"],
                        ]
                        for level in payload[side]
                    ]
                )
        elif kind == "Trade":
            trades["timestamp_ns"].append(record["timestamp_ns"])
            trades["sequence"].append(record["sequence"])
            trades["trade_id"].append(_safe(payload["id"]))
            trades["price_ticks"].append(_safe(payload["price_ticks"]))
            trades["quantity"].append(_safe(payload["quantity"]))
            trades["maker_agent_id"].append(payload["maker_agent_id"])
            trades["taker_agent_id"].append(payload["taker_agent_id"])
            is_buy = payload["aggressor_order_id"] == payload["buy_order_id"]
            trades["aggressor_side"].append("Buy" if is_buy else "Sell")
            trades["buy_order_id"].append(_safe(payload["buy_order_id"]))
            trades["sell_order_id"].append(_safe(payload["sell_order_id"]))
        elif kind == "ScenarioShock":
            shocks.append(
                {
                    "index": payload["index"],
                    "phase": payload["phase"],
                    "shock": payload["shock"],
                    "timestamp_ns": record["timestamp_ns"],
                    "sequence": record["sequence"],
                }
            )
        elif kind == "RunCompleted":
            terminal = payload

    for offset, length, record in _records(run_dir / "commands.jsonl"):
        market_index.add(0, offset, length, record)

    aligned = books["timestamp_ns"] == samples["timestamp_ns"]
    meta: Json = {
        "index_version": INDEX_VERSION,
        "run_id": manifest["run_id"],
        "agent_ids": agent_ids,
        "sample_count": len(samples["timestamp_ns"]),
        "book_count": len(books["timestamp_ns"]),
        "books_aligned_with_samples": aligned,
        "trade_count": len(trades["trade_id"]),
        "ledger_count": len(ledger["trade_id"]),
        "shocks": shocks,
        "units": accounting_header["units"],
        "accounting_header": accounting_header,
        "accounting_terminal": accounting_terminal,
        "terminal": terminal,
        "market_kinds": market_index.kinds,
        "accounting_kinds": accounting_index.kinds,
        "market_event_count": len(market_index.columns["sequence"]),
        "accounting_event_count": len(accounting_index.columns["sequence"]),
    }

    staging = cache_dir.with_name(cache_dir.name + ".building")
    if staging.exists():
        _remove_tree(staging)
    staging.mkdir(parents=True)
    _write_json(staging / "samples.json", samples)
    for agent, columns in agents.items():
        _write_json(staging / f"agent-{agent}.json", columns)
    _write_json(staging / "ledger.json", ledger)
    _write_json(staging / "trades.json", trades)
    _write_json(staging / "books.json", books)
    _write_arrays(staging / "market-events.bin", market_index.sorted_by_sequence(), {})
    _write_arrays(staging / "accounting-events.bin", accounting_index.columns, {})
    _write_json(staging / "meta.json", meta)
    if cache_dir.exists():
        _remove_tree(cache_dir)
    staging.rename(cache_dir)


def _remove_tree(path: Path) -> None:
    for child in path.iterdir():
        child.unlink()
    path.rmdir()


# ---- reading -----------------------------------------------------------------------------


class _PieceCache:
    """A few recently used parsed files, shared by every request."""

    def __init__(self, capacity: int) -> None:
        self._capacity = capacity
        self._items: OrderedDict[str, Any] = OrderedDict()
        self._lock = threading.Lock()

    def get(self, path: Path, load: Callable[[Path], Any]) -> Any:
        key = str(path)
        with self._lock:
            if key in self._items:
                self._items.move_to_end(key)
                return self._items[key]
        value = load(path)
        with self._lock:
            self._items[key] = value
            self._items.move_to_end(key)
            while len(self._items) > self._capacity:
                self._items.popitem(last=False)
        return value

    def forget(self, prefix: Path) -> None:
        with self._lock:
            for key in [key for key in self._items if key.startswith(str(prefix))]:
                del self._items[key]


_CACHE = _PieceCache(capacity=48)
_BUILD_LOCKS: dict[str, threading.Lock] = {}
_BUILD_LOCKS_GUARD = threading.Lock()


def _load_json(path: Path) -> Any:
    return json.loads(path.read_text())


def decimation_indices(
    primary: Sequence[int | None], max_points: int, keep: Iterable[int] = ()
) -> list[int] | None:
    """Indices to plot when a series is longer than ``max_points``, or None to plot it all.

    Every kept point is a recorded observation. Each bucket keeps its first and last sample
    and the samples where the primary series is lowest and highest, so an extreme inside a
    bucket is never hidden. ``keep`` forces further indices in, for shock times.
    """
    count = len(primary)
    if count <= max_points:
        return None
    buckets = max(1, max_points // 4)
    size = math.ceil(count / buckets)
    chosen = {0, count - 1, *(index for index in keep if 0 <= index < count)}
    for start in range(0, count, size):
        stop = min(count, start + size)
        chosen.add(start)
        chosen.add(stop - 1)
        low: tuple[int, int] | None = None
        high: tuple[int, int] | None = None
        for index in range(start, stop):
            value = primary[index]
            if value is None:
                continue
            if low is None or value < low[0]:
                low = (value, index)
            if high is None or value > high[0]:
                high = (value, index)
        if low is not None and high is not None:
            chosen.add(low[1])
            chosen.add(high[1])
    return sorted(chosen)


def _take(column: Sequence[Any], indices: list[int] | None) -> list[Any]:
    return list(column) if indices is None else [column[index] for index in indices]


class RunStore:
    """The derived index of one completed run."""

    def __init__(self, run_dir: Path, cache_dir: Path, max_samples: int) -> None:
        self.run_dir = run_dir
        self.cache_dir = cache_dir
        self._max_samples = max_samples

    # ---- lifecycle ----------------------------------------------------------------------

    def ensure_built(self) -> None:
        if self._current():
            return
        with _BUILD_LOCKS_GUARD:
            lock = _BUILD_LOCKS.setdefault(str(self.cache_dir), threading.Lock())
        with lock:
            if not self._current():
                _CACHE.forget(self.cache_dir)
                build_index(self.run_dir, self.cache_dir, self._max_samples)

    def _current(self) -> bool:
        meta = self.cache_dir / "meta.json"
        if not meta.exists():
            return False
        try:
            return bool(json.loads(meta.read_text()).get("index_version") == INDEX_VERSION)
        except (OSError, json.JSONDecodeError):
            return False

    def _piece(self, name: str) -> Any:
        self.ensure_built()
        return _CACHE.get(self.cache_dir / name, _load_json)

    def _arrays(self, name: str) -> dict[str, array[Any]]:
        self.ensure_built()
        columns: dict[str, array[Any]] = _CACHE.get(
            self.cache_dir / name, lambda path: _read_arrays(path)[0]
        )
        return columns

    # ---- whole documents ----------------------------------------------------------------

    @property
    def meta(self) -> Json:
        meta: Json = self._piece("meta.json")
        return meta

    def document(self, name: str) -> Json:
        if name not in ("config.json", "metrics.json", "manifest.json"):
            raise ArtifactError("NotFound", f"{name} is not a JSON document of a run")
        document: Json = _CACHE.get(self.run_dir / name, _load_json)
        return document

    def files(self) -> list[Json]:
        checksums = self.document("manifest.json")["artifacts"]
        result = []
        for name in RUN_FILES:
            path = self.run_dir / name
            result.append(
                {
                    "name": name,
                    "bytes": path.stat().st_size if path.exists() else None,
                    "sha256": checksums.get(name),
                }
            )
        return result

    # ---- samples ------------------------------------------------------------------------

    def sample_times(self) -> list[int]:
        times: list[int] = self._piece("samples.json")["timestamp_ns"]
        return times

    def index_at(self, time_ns: int) -> int | None:
        """The latest sample at or before ``time_ns``; None when there is none yet."""
        position = bisect_right(self.sample_times(), time_ns) - 1
        return position if position >= 0 else None

    def market_series(self, max_points: int) -> Json:
        samples: Columns = self._piece("samples.json")
        sources = samples["mark_source"]
        midpoint = MARK_SOURCES.index("Midpoint")
        mid = [
            mark if source == midpoint else None
            for mark, source in zip(samples["mark_twice_ticks"], sources, strict=True)
        ]
        indices = decimation_indices(mid, max_points, self._shock_sample_indices())
        columns = {name: _take(column, indices) for name, column in samples.items()}
        columns["mid_twice_ticks"] = _take(mid, indices)
        return {
            "sample_count": len(sources),
            "decimated": indices is not None,
            "decimation": None if indices is None else "min_max_per_bucket(mid_twice_ticks)",
            "index": indices if indices is not None else list(range(len(sources))),
            "mark_sources": list(MARK_SOURCES),
            "derived": ["mid_twice_ticks"],
            "columns": columns,
        }

    def agent_series(self, agent_id: int, max_points: int, initial_cash_minor: int) -> Json:
        if agent_id not in self.meta["agent_ids"]:
            raise ArtifactError("NotFound", f"the run has no agent {agent_id}")
        columns: Columns = dict(self._piece(f"agent-{agent_id}.json"))
        equity = columns["equity_minor"]
        # Equity is first observed at the start of the run, when the account holds only its
        # opening cash; the running peak starts there, as the core's does.
        peak = initial_cash_minor
        drawdown = []
        for value in equity:
            peak = max(peak, value)
            drawdown.append(peak - value)
        columns["drawdown_minor"] = drawdown
        indices = decimation_indices(equity, max_points, self._shock_sample_indices())
        times = self.sample_times()
        return {
            "agent_id": agent_id,
            "sample_count": len(equity),
            "decimated": indices is not None,
            "decimation": None if indices is None else "min_max_per_bucket(equity_minor)",
            "index": indices if indices is not None else list(range(len(equity))),
            "derived": ["drawdown_minor", "cum_bought", "cum_sold"],
            "columns": {
                "timestamp_ns": _take(times, indices),
                **{name: _take(column, indices) for name, column in columns.items()},
            },
        }

    def _shock_sample_indices(self) -> list[int]:
        times = self.sample_times()
        result: list[int] = []
        for shock in self.meta["shocks"]:
            position = bisect_right(times, shock["timestamp_ns"] - 1)
            result.extend((position - 1, position))
        return result

    def sample_window(self, start: int, count: int, agent_ids: Sequence[int]) -> Json:
        """Exact recorded state for samples ``[start, start + count)``: market, book, agents."""
        samples: Columns = self._piece("samples.json")
        total = len(samples["timestamp_ns"])
        start = max(0, min(start, total))
        stop = min(total, start + count)
        window = {name: column[start:stop] for name, column in samples.items()}
        books: Columns = self._piece("books.json")
        aligned = self.meta["books_aligned_with_samples"]
        book_rows: list[Json | None] = []
        for index in range(start, stop):
            position: int | None = index
            if not aligned:
                found = bisect_right(books["timestamp_ns"], samples["timestamp_ns"][index]) - 1
                position = found if found >= 0 else None
            book_rows.append(None if position is None else self._book(books, position))
        agents: dict[str, Columns] = {}
        for agent in agent_ids:
            if agent not in self.meta["agent_ids"]:
                raise ArtifactError("NotFound", f"the run has no agent {agent}")
            columns: Columns = self._piece(f"agent-{agent}.json")
            agents[str(agent)] = {name: column[start:stop] for name, column in columns.items()}
        return {
            "start": start,
            "count": stop - start,
            "sample_count": total,
            "mark_sources": list(MARK_SOURCES),
            "columns": window,
            "books": book_rows,
            "agents": agents,
        }

    @staticmethod
    def _book(books: Columns, position: int) -> Json:
        return {
            "timestamp_ns": books["timestamp_ns"][position],
            "sequence": books["sequence"][position],
            "bids": books["bids"][position],
            "asks": books["asks"][position],
        }

    def book_at(self, time_ns: int) -> Json | None:
        """The latest recorded snapshot at or before ``time_ns``, with its own timestamp."""
        books: Columns = self._piece("books.json")
        position = bisect_right(books["timestamp_ns"], time_ns) - 1
        if position < 0:
            return None
        return {"index": position, **self._book(books, position)}

    # ---- trades and ledger --------------------------------------------------------------

    def trades(
        self,
        from_ns: int | None,
        to_ns: int | None,
        agent_id: int | None,
        offset: int,
        limit: int,
    ) -> Json:
        trades: Columns = self._piece("trades.json")
        times = trades["timestamp_ns"]
        first = 0 if from_ns is None else bisect_right(times, from_ns - 1)
        last = len(times) if to_ns is None else bisect_right(times, to_ns)
        positions: Sequence[int] = range(first, last)
        if agent_id is not None:
            maker, taker = trades["maker_agent_id"], trades["taker_agent_id"]
            positions = [
                index for index in positions if maker[index] == agent_id or taker[index] == agent_id
            ]
        page = positions[offset : offset + limit]
        return {
            "total": len(positions),
            "offset": offset,
            "rows": [{name: column[index] for name, column in trades.items()} for index in page],
        }

    def agent_ledger(self, agent_id: int, offset: int, limit: int) -> Json:
        if agent_id not in self.meta["agent_ids"]:
            raise ArtifactError("NotFound", f"the run has no agent {agent_id}")
        ledger: Columns = self._piece("ledger.json")
        trades: Columns = self._piece("trades.json")
        buyers, sellers = ledger["buyer"], ledger["seller"]
        positions = [
            index
            for index in range(len(buyers))
            if buyers[index] == agent_id or sellers[index] == agent_id
        ]
        # Trade IDs run from one in exchange order, in both streams.
        makers = trades["maker_agent_id"] if len(trades["trade_id"]) == len(buyers) else None
        rows = []
        for index in positions[offset : offset + limit]:
            side = "buyer" if buyers[index] == agent_id else "seller"
            other = "seller" if side == "buyer" else "buyer"
            role = None
            if makers is not None:
                role = "Maker" if makers[index] == agent_id else "Taker"
            rows.append(
                {
                    "timestamp_ns": ledger["timestamp_ns"][index],
                    "sequence": ledger["sequence"][index],
                    "trade_id": ledger["trade_id"][index],
                    "side": "Buy" if side == "buyer" else "Sell",
                    "role": role,
                    "quantity": ledger["quantity"][index],
                    "price_ticks": ledger["price_ticks"][index],
                    "notional_minor": ledger["notional_minor"][index],
                    "fee_minor": ledger[f"{side}_fee_minor"][index],
                    "cash_minor": ledger[f"{side}_cash_minor"][index],
                    "position": ledger[f"{side}_position"][index],
                    "cost_basis_minor": ledger[f"{side}_cost_basis_minor"][index],
                    "realized_pnl_minor": ledger[f"{side}_realized_pnl_minor"][index],
                    "counterparty_agent_id": ledger[other][index],
                }
            )
        return {"total": len(positions), "offset": offset, "rows": rows}

    # ---- records ------------------------------------------------------------------------

    def _event_positions(
        self,
        stream: str,
        channel: str | None,
        kinds: Sequence[str] | None,
        agent_id: int | None,
        order_id: int | None,
    ) -> tuple[dict[str, array[Any]], list[str], Sequence[int]]:
        market = stream == "market"
        columns = self._arrays("market-events.bin" if market else "accounting-events.bin")
        names: list[str] = self.meta["market_kinds" if market else "accounting_kinds"]
        count = len(columns["sequence"])
        if not market:
            channel = agent_id = order_id = None
        if channel is None and not kinds and agent_id is None and order_id is None:
            return columns, names, range(count)
        wanted_channel = None if channel is None else CHANNELS.index(channel)
        wanted_kinds = None if not kinds else {names.index(kind) for kind in kinds if kind in names}
        channels, kind_codes = columns["channel"], columns["kind"]
        actors, others, orders = columns["actor"], columns["other"], columns["order"]
        positions = [
            index
            for index in range(count)
            if (wanted_channel is None or channels[index] == wanted_channel)
            and (wanted_kinds is None or kind_codes[index] in wanted_kinds)
            and (agent_id is None or actors[index] == agent_id or others[index] == agent_id)
            and (order_id is None or orders[index] == order_id)
        ]
        return columns, names, positions

    def events(
        self,
        stream: str,
        channel: str | None,
        kinds: Sequence[str] | None,
        agent_id: int | None,
        order_id: int | None,
        offset: int,
        limit: int,
        at_or_before_ns: int | None = None,
        at_or_before_sequence: int | None = None,
    ) -> Json:
        """A bounded page of records in sequence order.

        With ``at_or_before_ns`` or ``at_or_before_sequence`` the page is positioned so that
        its last row is the latest matching record at or before that point, and ``anchor``
        is that row's offset among the matching records.
        """
        columns, names, positions = self._event_positions(
            stream, channel, kinds, agent_id, order_id
        )
        total = len(positions)
        anchor: int | None = None
        if at_or_before_ns is not None or at_or_before_sequence is not None:
            key = "timestamp_ns" if at_or_before_ns is not None else "sequence"
            bound = at_or_before_ns if at_or_before_ns is not None else at_or_before_sequence
            column = columns[key]
            found = bisect_right(positions, bound, key=lambda index: column[index]) - 1
            anchor = found if found >= 0 else None
            offset = max(0, (found + 1) - limit) if found >= 0 else 0
        page = positions[offset : offset + limit]
        market = stream == "market"
        handles = {
            0: (self.run_dir / ("commands.jsonl" if market else "accounting.jsonl")).open("rb"),
            1: (self.run_dir / "trace.jsonl").open("rb") if market else None,
        }
        try:
            rows = []
            for index in page:
                source = handles[columns["channel"][index]]
                assert source is not None
                source.seek(columns["offset"][index])
                raw = source.read(columns["length"][index])
                rows.append(
                    _event_row(
                        json.loads(raw),
                        CHANNELS[columns["channel"][index]] if market else "accounting",
                    )
                )
        finally:
            for handle in handles.values():
                if handle is not None:
                    handle.close()
        return {
            "stream": stream,
            "total": total,
            "offset": offset,
            "anchor": anchor,
            "kinds": names,
            "rows": rows,
        }


def _event_row(record: Json, channel: str) -> Json:
    payload = record.get("payload") or {}
    kind = record["kind"]
    price = None
    if kind == "OrderArrival":
        price = payload.get("limit_price_ticks") if payload.get("type") == "Limit" else None
    elif kind == "Trade":
        price = payload.get("price_ticks")
    order_id = record.get("correlation_id")
    return {
        "channel": channel,
        "sequence": record["sequence"],
        "timestamp_ns": record["timestamp_ns"],
        "kind": kind,
        "actor_id": record.get("actor_id"),
        "order_id": order_id,
        "causation_id": record.get("causation_id"),
        "scheduler_sequence": record.get("scheduler_sequence"),
        "price_ticks": price,
        "quantity": payload.get("quantity"),
        "side": payload.get("side"),
        "state": payload.get("state"),
        "reason": payload.get("reason") if payload.get("reason") not in (None, "None") else None,
        "payload": payload,
        "payload_digest": record.get("payload_digest"),
    }
