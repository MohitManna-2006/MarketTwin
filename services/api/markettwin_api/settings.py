"""Service configuration, read once from the environment."""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path

_REPOSITORY = Path(__file__).resolve().parents[3]

HOUR_NS = 3_600_000_000_000


@dataclass(frozen=True)
class Settings:
    """Roots and budgets of one API process.

    The workspace is the only place the service writes, and the scenario directory the
    only place it reads configs from. Neither is ever taken from a request.
    """

    workspace: Path = _REPOSITORY / "data" / "research"
    scenarios_dir: Path = _REPOSITORY / "configs"
    max_concurrent_jobs: int = 2
    max_queued_jobs: int = 16
    max_request_bytes: int = 2 * 1024 * 1024
    # Budgets the API applies on top of the core's own validation.
    max_horizon_ns: int = 24 * HOUR_NS
    max_events: int = 100_000_000
    max_trace_bytes: int = 2 * 1024 * 1024 * 1024
    max_experiment_runs: int = 2_000
    max_experiment_output_bytes: int = 8 * 1024 * 1024 * 1024
    max_samples: int = 500_000
    ai_provider: str = "openai_compatible"
    ai_model: str = ""
    ai_base_url: str = "https://api.openai.com/v1"
    ai_api_key: str = ""
    ai_timeout_seconds: float = 30.0
    ai_input_usd_per_million_tokens: str = ""
    ai_output_usd_per_million_tokens: str = ""
    ai_max_seeds: int = 100
    ai_max_investigations: int = 1000
    ai_max_concurrent_investigations: int = 32
    cors_origins: tuple[str, ...] = field(
        default=("http://127.0.0.1:5173", "http://localhost:5173")
    )

    @property
    def runs_dir(self) -> Path:
        return self.workspace / "runs"

    @property
    def experiments_dir(self) -> Path:
        return self.workspace / "experiments"

    @property
    def derived_dir(self) -> Path:
        return self.workspace / "derived"

    @property
    def registry_path(self) -> Path:
        return self.workspace / "registry.sqlite3"


def load_settings() -> Settings:
    defaults = Settings()
    origins = os.environ.get("MARKETTWIN_CORS_ORIGINS")
    return Settings(
        workspace=Path(os.environ.get("MARKETTWIN_WORKSPACE", defaults.workspace)).resolve(),
        scenarios_dir=Path(
            os.environ.get("MARKETTWIN_SCENARIOS", defaults.scenarios_dir)
        ).resolve(),
        max_concurrent_jobs=int(
            os.environ.get("MARKETTWIN_MAX_CONCURRENT_JOBS", defaults.max_concurrent_jobs)
        ),
        max_queued_jobs=int(os.environ.get("MARKETTWIN_MAX_QUEUED_JOBS", defaults.max_queued_jobs)),
        ai_provider=os.environ.get("MARKETTWIN_AI_PROVIDER", defaults.ai_provider),
        ai_model=os.environ.get("MARKETTWIN_AI_MODEL", defaults.ai_model),
        ai_base_url=os.environ.get("MARKETTWIN_AI_BASE_URL", defaults.ai_base_url),
        ai_api_key=os.environ.get("MARKETTWIN_AI_API_KEY", ""),
        ai_timeout_seconds=float(
            os.environ.get("MARKETTWIN_AI_TIMEOUT_SECONDS", defaults.ai_timeout_seconds)
        ),
        ai_input_usd_per_million_tokens=os.environ.get(
            "MARKETTWIN_AI_INPUT_USD_PER_MILLION_TOKENS", ""
        ),
        ai_output_usd_per_million_tokens=os.environ.get(
            "MARKETTWIN_AI_OUTPUT_USD_PER_MILLION_TOKENS", ""
        ),
        ai_max_seeds=int(os.environ.get("MARKETTWIN_AI_MAX_SEEDS", defaults.ai_max_seeds)),
        cors_origins=tuple(origins.split(",")) if origins else defaults.cors_origins,
    )
