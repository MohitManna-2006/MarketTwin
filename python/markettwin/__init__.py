"""Run-level Python interface to the MarketTwin C++20 simulation core.

The core owns matching, logical time, agents, accounting, and statistics. This package
starts whole runs and reads back what they wrote; it never computes a price, a fill, or a
P&L figure itself.
"""

from markettwin.native import (
    ExperimentOutput,
    ExperimentProgress,
    LiveNormalizer,
    NativeRunError,
    build_info,
    calibration_observed_facts,
    calibration_score,
    calibration_synthetic_facts,
    calibration_targets,
    canonicalize_config,
    canonicalize_experiment,
    experiment_cell_config,
    fork_config,
    import_historical_jsonl,
    run,
    run_experiment,
    verify_fork,
    verify_historical,
    verify_replay,
    version,
)

__all__ = [
    "ExperimentOutput",
    "ExperimentProgress",
    "LiveNormalizer",
    "NativeRunError",
    "build_info",
    "calibration_observed_facts",
    "calibration_score",
    "calibration_synthetic_facts",
    "calibration_targets",
    "canonicalize_config",
    "canonicalize_experiment",
    "experiment_cell_config",
    "fork_config",
    "import_historical_jsonl",
    "run",
    "run_experiment",
    "verify_fork",
    "verify_historical",
    "verify_replay",
    "version",
]
