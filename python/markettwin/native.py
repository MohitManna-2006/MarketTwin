"""Typed wrappers over the extension module.

JSON text crosses the native boundary; these functions parse it once per call. Every call
is one complete native operation with the GIL released, so they are safe to run from
worker threads, and independent runs may execute concurrently.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from os import PathLike, fspath
from typing import Any

from markettwin import _native

NativeRunError = _native.NativeRunError
ExperimentProgress = _native.ExperimentProgress

JsonObject = dict[str, Any]


def version() -> str:
    """Engine version of the linked core, for example ``0.4.0``."""
    return _native.version()


def build_info() -> JsonObject:
    """Engine version, build provenance, and the schema versions the core writes."""
    return dict(_native.build_info())


def canonicalize_config(config: JsonObject | str) -> tuple[str, str]:
    """Validate a run config natively. Returns its canonical JSON text and config digest.

    Raises NativeRunError (code ``InvalidConfig``, ``InvalidJson``, ``UnsupportedSchema``,
    ...) exactly as the CLI would.
    """
    canonical, digest = _native.canonicalize_config(_text(config))
    return canonical, digest


def run(
    config: JsonObject | str, output_directory: str | PathLike[str] | None = None
) -> JsonObject:
    """Run one simulation and return its terminal summary.

    With ``output_directory`` (which must not exist) the six run artifacts are written
    there; without it the run stays in memory.
    """
    directory = "" if output_directory is None else fspath(output_directory)
    return json.loads(_native.run(_text(config), directory))


def verify_replay(run_directory: str | PathLike[str]) -> JsonObject:
    """Regenerate a recorded run and byte-compare every record against its artifacts."""
    return json.loads(_native.verify_replay(fspath(run_directory)))


def import_historical_jsonl(
    input_path: str | PathLike[str], output_directory: str | PathLike[str], metadata: JsonObject
) -> JsonObject:
    """Normalize one observed-market file without entering the synthetic exchange."""
    return json.loads(
        _native.import_historical_jsonl(
            fspath(input_path), fspath(output_directory), _text(metadata)
        )
    )


def verify_historical(session_directory: str | PathLike[str]) -> JsonObject:
    """Check historical artifact digests and regenerate L1 playback observations."""
    return json.loads(_native.verify_historical(fspath(session_directory)))


def calibration_observed_facts(
    session_directory: str | PathLike[str], start_ns: int, end_ns: int
) -> JsonObject:
    """Stylized facts of a verified historical session inside [start_ns, end_ns)."""
    return json.loads(
        _native.calibration_observed_facts(fspath(session_directory), start_ns, end_ns)
    )


def calibration_synthetic_facts(config: JsonObject | str) -> JsonObject:
    """Run a config in memory and measure the same stylized facts on its L1 projection."""
    return json.loads(_native.calibration_synthetic_facts(_text(config)))


def calibration_targets(facts: list[JsonObject]) -> JsonObject:
    """Mean, deviation, and floored scale of each fact across sessions."""
    return json.loads(_native.calibration_targets(json.dumps(facts)))


def calibration_score(target: JsonObject, scale: JsonObject, facts: list[JsonObject]) -> JsonObject:
    """Distances and group-weighted score of one candidate's per-seed facts."""
    return json.loads(
        _native.calibration_score(json.dumps(target), json.dumps(scale), json.dumps(facts))
    )


class LiveNormalizer:
    """Native normalizer and per-instrument state of one live feed.

    Provider messages go in as whole batches with prices already rendered as decimal
    strings; the core does every conversion, classification, and state update. Python
    never touches a price and is never called per record.
    """

    def __init__(self, config: JsonObject) -> None:
        self._native = _native.LiveNormalizer(_text(config))

    def process_batch(self, batch: JsonObject | str) -> JsonObject:
        """Normalize one batch of frames; returns events, rejects, and changed states."""
        return json.loads(self._native.process_batch(_text(batch)))

    def process_batch_text(self, batch: str) -> str:
        """The same call without parsing the result, for callers that forward the text."""
        return self._native.process_batch(batch)

    def snapshot(self, now_wall_ns: int, now_monotonic_ns: int) -> JsonObject:
        """Every instrument's state and freshness as of the caller's clocks."""
        return json.loads(self._native.snapshot(now_wall_ns, now_monotonic_ns))

    def retain_symbols(self, symbols: list[str]) -> None:
        """Forget every instrument that is no longer subscribed."""
        self._native.retain_symbols(json.dumps(symbols))

    @property
    def last_batch_elapsed_ns(self) -> int:
        """Steady-clock duration of the last native batch, measured inside the core."""
        return int(self._native.last_batch_elapsed_ns)


def canonicalize_experiment(
    config: JsonObject | str, base_directory: str | PathLike[str] | None = None
) -> str:
    """Validate an experiment config natively and return its canonical JSON text."""
    return _native.canonicalize_experiment(_text(config), _directory(base_directory))


def experiment_cell_config(
    config: JsonObject | str,
    variant: str,
    seed: int,
    base_directory: str | PathLike[str] | None = None,
) -> str:
    """Canonical run config of one (variant, seed) cell: run alone, it gives that cell's digests."""
    return _native.experiment_cell_config(_text(config), _directory(base_directory), variant, seed)


@dataclass(frozen=True)
class ExperimentOutput:
    """What one experiment returns: the native results document and its per-seed values."""

    results: JsonObject
    paired_values: JsonObject


def run_experiment(
    config: JsonObject | str,
    output_directory: str | PathLike[str] | None = None,
    progress: ExperimentProgress | None = None,
    base_directory: str | PathLike[str] | None = None,
) -> ExperimentOutput:
    """Run every cell of an experiment on the calling thread.

    ``progress`` is updated from native code and may be read, or asked to cancel, from
    another thread while this call is running.
    """
    results, values = _native.run_experiment(
        _text(config), _directory(base_directory), _directory(output_directory), progress
    )
    return ExperimentOutput(results=json.loads(results), paired_values=json.loads(values))


def _text(config: JsonObject | str) -> str:
    return config if isinstance(config, str) else json.dumps(config)


def _directory(path: str | PathLike[str] | None) -> str:
    return "" if path is None else fspath(path)
