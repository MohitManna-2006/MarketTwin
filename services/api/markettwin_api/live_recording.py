"""Durable capture of a live stream and its bridge into Phase 4A historical sessions.

A capture is one append-only JSON-lines file. Every provider data frame is written
exactly as received, in arrival order, with the wall-clock and monotonic receipt times
beside it, before any other processing sees it. Connection lifecycle changes are written
into the same file, so an upstream interruption sits in the evidence between the frames
it separates rather than in a log somewhere else.

    {"type":"header", ...}                      one, first
    {"type":"frame","o":1,"raw":"[...]", ...}    provider payload, verbatim
    {"type":"lifecycle","event":"upstream_lost"} connection changes
    {"type":"footer", ...}                      one, last; absent after a crash

Nothing in a capture is rewritten. Finalizing reads it back, lets the native core decide
which records are valid, and imports one ordinary historical session per instrument
through the Phase 4A importer, which normalizes, orders, digests, and verifies it. A
capture without a footer was interrupted; it is finalized from what reached the disk.

A recording holds what this connection delivered. It is never presented as a complete
record of the market: the feed is one venue or a test stream, and any interruption is
listed as a coverage gap.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import time
from collections import Counter
from collections.abc import Iterator
from datetime import UTC, datetime
from decimal import Decimal
from pathlib import Path
from typing import IO, Any

import markettwin

from markettwin_api.historical_providers import Acquisition
from markettwin_api.historical_service import HistoricalResearch
from markettwin_api.live_gateway import Frame, render_prices
from markettwin_api.registry import now

Json = dict[str, Any]

CAPTURE_SCHEMA = 1
CAPTURE_FILE = "capture.jsonl"
SUMMARY_FILE = "summary.json"
RECORDING_ID = re.compile(r"rec-[a-f0-9]{10}\Z")
DISCLAIMER = (
    "A recording holds only what this connection delivered. It is not a complete record "
    "of trading in the instrument: the feed covers one venue or a test stream, records the "
    "provider did not send are absent, and every interruption is listed as a coverage gap."
)
# Provider fields the Phase 4A normalizer reads, in the adapter's order.
_KEPT = ("t", "p", "s", "i", "x", "c", "bp", "ap", "bs", "as", "bx", "ax", "z")
_VALIDATION_BATCH = 2_000


def iso(wall_ns: int) -> str:
    return (
        datetime.fromtimestamp(wall_ns / 1e9, UTC)
        .isoformat(timespec="milliseconds")
        .replace("+00:00", "Z")
    )


def _number(value: Any) -> Any:
    # Only non-price numbers reach this: prices are already decimal strings. The core
    # refuses a fractional size, so the float is never used as a quantity.
    if isinstance(value, Decimal):
        return float(value)
    raise TypeError(f"cannot encode {type(value).__name__}")


def encode_batch(frames: list[tuple[int, int, int, list[Any]]]) -> str:
    """The native batch for ``(number, receipt wall ns, receipt monotonic ns, messages)``."""
    return json.dumps(
        {
            "now_wall_ns": str(time.time_ns()),
            "now_monotonic_ns": time.monotonic_ns(),
            "frames": [
                {
                    "frame": number,
                    "receipt_wall_ns": str(wall),
                    "receipt_monotonic_ns": monotonic,
                    "messages": messages,
                }
                for number, wall, monotonic, messages in frames
            ],
        },
        separators=(",", ":"),
        default=_number,
    )


class CaptureWriter:
    """Appends to one capture file. Every line is flushed to the operating system as it is
    written, and synced to the device on a timer and at close."""

    def __init__(
        self,
        directory: Path,
        header: Json,
        max_bytes: int,
        max_messages: int,
        max_seconds: float,
    ) -> None:
        directory.mkdir(parents=True, exist_ok=False)
        self.directory = directory
        self.path = directory / CAPTURE_FILE
        self._file: IO[bytes] | None = self.path.open("xb")
        self._max_bytes = max_bytes
        self._max_messages = max_messages
        self._deadline = time.monotonic() + max_seconds
        self.frames = 0
        self.messages = 0
        self.bytes = 0
        self.lifecycle_events = 0
        self.started_monotonic = time.monotonic()
        self._write({"type": "header", "capture_schema": CAPTURE_SCHEMA, **header})
        self.sync()

    def _write(self, line: Json) -> None:
        if self._file is None:
            return
        data = json.dumps(line, separators=(",", ":")).encode() + b"\n"
        self._file.write(data)
        self._file.flush()
        self.bytes += len(data)

    def frame(self, frame: Frame) -> str | None:
        """Append one provider frame. Returns the reason the capture must now stop, if a
        storage bound was reached; the frame that reached it is kept."""
        self.frames += 1
        self.messages += len(frame.messages)
        self._write(
            {
                "type": "frame",
                "o": self.frames,
                "n": frame.number,
                "receipt_wall_ns": str(frame.receipt_wall_ns),
                "receipt_monotonic_ns": frame.receipt_monotonic_ns,
                "raw": frame.text,
            }
        )
        if self.bytes >= self._max_bytes:
            return "storage_limit"
        if self.messages >= self._max_messages:
            return "message_limit"
        return "duration_limit" if time.monotonic() >= self._deadline else None

    def lifecycle(self, event: str, **fields: Any) -> None:
        self.lifecycle_events += 1
        wall = time.time_ns()
        self._write(
            {"type": "lifecycle", "event": event, "wall_ns": str(wall), "at": iso(wall), **fields}
        )

    def expired(self) -> bool:
        return time.monotonic() >= self._deadline

    def sync(self) -> None:
        if self._file is not None:
            self._file.flush()
            os.fsync(self._file.fileno())

    def close(self, reason: str) -> None:
        if self._file is None:
            return
        wall = time.time_ns()
        self._write(
            {
                "type": "footer",
                "stop_reason": reason,
                "stopped_wall_ns": str(wall),
                "stopped_at": iso(wall),
                "frames": self.frames,
                "messages": self.messages,
            }
        )
        self.sync()
        self._file.close()
        self._file = None

    def stats(self) -> Json:
        return {
            "frames": self.frames,
            "messages": self.messages,
            "bytes": self.bytes,
            "max_bytes": self._max_bytes,
            "max_messages": self._max_messages,
            "elapsed_ms": round((time.monotonic() - self.started_monotonic) * 1000),
        }


def _lines(path: Path) -> Iterator[tuple[Json | None, bool]]:
    """Each line as ``(record, complete)``. A last line without its newline was being
    written when the process stopped; it is reported, never repaired."""
    with path.open("rb") as source:
        for data in source:
            complete = data.endswith(b"\n")
            try:
                record = json.loads(data)
            except (ValueError, UnicodeDecodeError):
                record = None
            yield (record if isinstance(record, dict) else None), complete


def _historical_record(message: Json) -> str:
    record = {key: message[key] for key in _KEPT if message.get(key) is not None}
    record["kind"] = "trade" if message["T"] == "t" else "quote"
    record["symbol"] = message["S"]
    return json.dumps(record, separators=(",", ":"), default=_number) + "\n"


class _Reader:
    """One pass over a capture: lifecycle, native validation, and per-instrument input."""

    def __init__(self, directory: Path, provider: str, feed: str) -> None:
        self.directory = directory
        self.staging = directory / "symbols"
        self.staging.mkdir(exist_ok=True)
        self.normalizer = markettwin.LiveNormalizer(
            {"provider": provider, "feed": feed, "max_symbols": 4096, "duplicate_window": 0}
        )
        self.outputs: dict[str, IO[str]] = {}
        self.records: Counter[str] = Counter()
        self.reasons: Counter[str] = Counter()
        self.unsupported: Counter[str] = Counter()
        self.pending: list[tuple[int, int, int, list[Any]]] = []
        self.pending_messages = 0
        self.rejected = 0
        self.unparsed_frames = 0

    def add(self, ordinal: int, wall: int, monotonic: int, raw: str) -> None:
        try:
            messages = json.loads(raw, parse_float=Decimal)
        except ValueError:
            messages = None
        if not isinstance(messages, list):
            self.unparsed_frames += 1
            return
        render_prices(messages)
        self.pending.append((ordinal, wall, monotonic, messages))
        self.pending_messages += len(messages)
        if self.pending_messages >= _VALIDATION_BATCH:
            self.flush()

    def flush(self) -> None:
        if not self.pending:
            return
        result = self.normalizer.process_batch(encode_batch(self.pending))
        refused = {(item["frame"], item["index"]) for item in result["rejects"]}
        self.rejected += len(refused)
        for item in result["rejects"]:
            self.reasons[item["reason"]] += 1
        for kind, count in result["unsupported"].items():
            self.unsupported[kind] += count
        for ordinal, _, _, messages in self.pending:
            for index, message in enumerate(messages):
                if (
                    isinstance(message, dict)
                    and message.get("T") in ("t", "q")
                    and (ordinal, index) not in refused
                ):
                    symbol = message["S"]
                    output = self.outputs.get(symbol)
                    if output is None:
                        output = (self.staging / f"{symbol}.jsonl").open("w", encoding="utf-8")
                        self.outputs[symbol] = output
                    output.write(_historical_record(message))
                    self.records[symbol] += 1
        self.pending = []
        self.pending_messages = 0

    def close(self) -> None:
        self.flush()
        for output in self.outputs.values():
            output.close()


def _gaps(
    lifecycle: list[Json], end_wall: int | None, footer: Json | None
) -> tuple[list[Json], list[Json]]:
    gaps: list[Json] = []
    transitions: list[Json] = []
    opened: Json | None = None

    def close(to_wall: int | None, closed_by: str) -> None:
        nonlocal opened
        if opened is None:
            return
        start = int(opened["wall_ns"])
        gaps.append(
            {
                "from_wall_ns": str(start),
                "from": iso(start),
                "to_wall_ns": None if to_wall is None else str(to_wall),
                "to": None if to_wall is None else iso(to_wall),
                "duration_ms": None if to_wall is None else max(0, (to_wall - start) // 1_000_000),
                "reason": opened.get("detail") or opened["event"],
                "closed_by": closed_by,
            }
        )
        opened = None

    for event in lifecycle:
        if event["event"] == "upstream_lost" and opened is None:
            opened = event
        elif event["event"] == "upstream_restored":
            close(int(event["wall_ns"]), "upstream_restored")
        if event["event"] == "state" and len(transitions) < 200:
            transitions.append(
                {key: event.get(key) for key in ("at", "from_state", "to_state", "detail")}
            )
    close(end_wall, "recording_stopped" if footer else "capture_ended")
    return gaps, transitions


def finalize_capture(directory: Path, historical: HistoricalResearch) -> Json:
    """Turn a closed or interrupted capture into verified historical sessions.

    Runs on a worker thread. It is idempotent: a capture that was already finalized
    returns its stored summary, and an instrument whose session already completed is not
    imported again.
    """
    summary_path = directory / SUMMARY_FILE
    if summary_path.is_file():
        stored: Json = json.loads(summary_path.read_text())
        return stored
    capture = directory / CAPTURE_FILE
    header: Json | None = None
    footer: Json | None = None
    lifecycle: list[Json] = []
    truncated_tail = False
    corrupt_lines = 0
    frames = 0
    first_wall: int | None = None
    last_wall: int | None = None
    reader: _Reader | None = None
    digest = hashlib.sha256()
    with capture.open("rb") as source:
        while chunk := source.read(1 << 20):
            digest.update(chunk)
    try:
        for record, complete in _lines(capture):
            if record is None or not complete:
                if complete:
                    corrupt_lines += 1
                else:
                    truncated_tail = True
                continue
            kind = record.get("type")
            if kind == "header" and header is None:
                header = record
                reader = _Reader(directory, record["provider"], record["feed"])
            elif kind == "frame" and reader is not None:
                wall = int(record["receipt_wall_ns"])
                first_wall = wall if first_wall is None else first_wall
                last_wall = wall
                frames += 1
                reader.add(record["o"], wall, record["receipt_monotonic_ns"], record["raw"])
            elif kind == "lifecycle":
                lifecycle.append(record)
                last_wall = int(record["wall_ns"])
            elif kind == "footer":
                footer = record
        if header is None or reader is None:
            raise ValueError("the capture has no header")
        reader.close()
    finally:
        if reader is not None:
            for output in reader.outputs.values():
                output.close()

    head: Json = header
    started_wall = int(head["started_wall_ns"])
    end_wall = int(footer["stopped_wall_ns"]) if footer else last_wall
    gaps, transitions = _gaps(lifecycle, end_wall, footer)
    stop_reason = footer["stop_reason"] if footer else "process_interrupted"
    if footer is None:
        # What the stream carried after the last line reached the disk is unknown.
        tail = end_wall or started_wall
        gaps.append(
            {
                "from_wall_ns": str(tail),
                "from": iso(tail),
                "to_wall_ns": None,
                "to": None,
                "duration_ms": None,
                "reason": "the API process stopped while recording",
                "closed_by": "process_interrupted",
            }
        )
    started_at = head["started_at"]
    stopped_at = footer["stopped_at"] if footer else iso(end_wall or started_wall)
    accepted = sum(reader.records.values())
    complete = not gaps and not truncated_tail and corrupt_lines == 0 and footer is not None
    capture_facts: Json = {
        "recording_id": head["recording_id"],
        "capture_schema": head["capture_schema"],
        "capture_sha256": digest.hexdigest(),
        "source_label": head["source_label"],
        "test_data": head["test_data"],
        "endpoint_host": head.get("endpoint_host"),
        "subscribed_symbols": head.get("symbols", []),
        "started_at": started_at,
        "stopped_at": stopped_at,
        "stop_reason": stop_reason,
        "frames": frames,
        "accepted_records": accepted,
        "rejected_records": reader.rejected,
        "unsupported_messages": dict(reader.unsupported),
        "unparsed_frames": reader.unparsed_frames,
        "truncated_tail": truncated_tail,
        "corrupt_lines": corrupt_lines,
        "first_receipt_wall_ns": None if first_wall is None else str(first_wall),
        "last_receipt_wall_ns": None if last_wall is None else str(last_wall),
        "coverage_gaps": gaps,
        "uninterrupted": complete,
        "disclaimer": DISCLAIMER,
    }

    symbols: dict[str, Json] = {}
    for symbol in sorted(reader.records):
        staged = reader.staging / f"{symbol}.jsonl"

        def prepare(raw_dir: Path, staged: Path = staged, symbol: str = symbol) -> Acquisition:
            raw_dir.mkdir(parents=True, exist_ok=False)
            retained = raw_dir / "records.jsonl"
            staged.replace(retained)
            return Acquisition(
                retained,
                {
                    "symbol": symbol,
                    "provider": head["provider"],
                    "feed": head["feed"],
                    "scope": head["scope"],
                    "license": head["license"],
                    "acquired_at": now(),
                    "requested_start": started_at,
                    "requested_end": stopped_at,
                    "raw_source_sha256": hashlib.sha256(retained.read_bytes()).hexdigest(),
                    "capture": {**capture_facts, "records_for_symbol": reader.records[symbol]},
                },
            )

        try:
            row = historical.import_prepared(
                head["provider"],
                head["feed"],
                symbol,
                started_at,
                stopped_at,
                head["recording_id"],
                prepare,
            )
            symbols[symbol] = {
                "records": reader.records[symbol],
                "session_id": row["id"],
                "status": row["status"],
                "error_code": row["error_code"],
            }
        except Exception as error:
            symbols[symbol] = {
                "records": reader.records[symbol],
                "session_id": None,
                "status": "failed",
                "error_code": type(error).__name__,
            }
    with_sessions = [
        item["session_id"] for item in symbols.values() if item["status"] == "completed"
    ]
    summary: Json = {
        **capture_facts,
        "provider": head["provider"],
        "feed": head["feed"],
        "bytes": capture.stat().st_size,
        "reject_reasons": dict(reader.reasons.most_common(20)),
        "state_transitions": transitions,
        "symbols": symbols,
        "sessions": with_sessions,
    }
    temporary = directory / (SUMMARY_FILE + ".partial")
    with temporary.open("w", encoding="utf-8") as output:
        json.dump(summary, output, separators=(",", ":"))
        output.flush()
        os.fsync(output.fileno())
    temporary.replace(summary_path)
    return summary
