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
