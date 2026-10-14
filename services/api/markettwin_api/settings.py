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
    historical_sources_dir: Path = Path(__file__).resolve().parent / "fixtures" / "historical"
    alpaca_key_id: str = field(default="", repr=False)
    alpaca_secret_key: str = field(default="", repr=False)
    historical_max_pages: int = 20
    # ---- live market data (Phase 4B) ----------------------------------------------------
    # The feed a start request without one selects: "test", "iex", or "mock".
    live_feed: str = "iex"
    # Connect at service start. Off by default: most Alpaca plans allow one connection per
    # endpoint, and a second MarketTwin process must not take it by accident.
    live_autostart: bool = False
    live_symbols: tuple[str, ...] = ("AAPL", "NVDA", "MSFT", "SPY")
    # Subscription ceiling this service enforces before the provider does. Alpaca's free
    # plan documents 30 symbols on the stock stream; raise it only with a larger plan.
    live_max_symbols: int = 30
    # Loopback URL of a mock Alpaca stream, for the "mock" feed only.
    live_mock_url: str = ""
    live_connect_timeout_seconds: float = 10.0
    live_handshake_timeout_seconds: float = 10.0
    live_backoff_base_seconds: float = 1.0
    live_backoff_max_seconds: float = 60.0
    # A connection that lasted this long resets the backoff schedule.
    live_stable_after_seconds: float = 30.0
    # Local coalescing window: frames received within it share one native call.
    live_flush_interval_seconds: float = 0.02
    live_max_batch_messages: int = 4_096
    live_inbox_frames: int = 4_096
    live_stale_after_seconds: float = 15.0
    live_skew_alert_seconds: float = 10.0
    live_heartbeat_seconds: float = 1.0
    live_max_clients: int = 16
    live_client_queue_messages: int = 256
    live_send_timeout_seconds: float = 5.0
    live_recent_events_per_symbol: int = 256
    live_recording_max_bytes: int = 64 * 1024 * 1024
    live_recording_max_messages: int = 400_000
    live_recording_max_seconds: float = 8 * 3600.0
    live_recording_sync_seconds: float = 1.0
    # Shared secret for live control requests that do not come from this machine.
    live_control_token: str = field(default="", repr=False)
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

    @property
    def historical_dir(self) -> Path:
        return self.workspace / "historical"

    @property
    def live_dir(self) -> Path:
        return self.workspace / "live"


def load_settings() -> Settings:
    defaults = Settings()
    origins = os.environ.get("MARKETTWIN_CORS_ORIGINS")
    symbols = os.environ.get("MARKETTWIN_LIVE_SYMBOLS")
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
        historical_sources_dir=Path(
            os.environ.get("MARKETTWIN_HISTORICAL_SOURCES", defaults.historical_sources_dir)
        ).resolve(),
        alpaca_key_id=os.environ.get("ALPACA_API_KEY_ID", ""),
        alpaca_secret_key=os.environ.get("ALPACA_API_SECRET_KEY", ""),
        historical_max_pages=int(os.environ.get("MARKETTWIN_HISTORICAL_MAX_PAGES", "20")),
        live_feed=os.environ.get("MARKETTWIN_LIVE_FEED", defaults.live_feed).strip().lower(),
        live_autostart=os.environ.get("MARKETTWIN_LIVE_AUTOSTART", "").strip().lower()
        in ("1", "true", "yes", "on"),
        live_symbols=tuple(item.strip().upper() for item in symbols.split(",") if item.strip())
        if symbols
        else defaults.live_symbols,
        live_max_symbols=int(
            os.environ.get("MARKETTWIN_LIVE_MAX_SYMBOLS", defaults.live_max_symbols)
        ),
        live_mock_url=os.environ.get("MARKETTWIN_LIVE_MOCK_URL", ""),
        live_backoff_base_seconds=float(
            os.environ.get(
                "MARKETTWIN_LIVE_BACKOFF_BASE_SECONDS", defaults.live_backoff_base_seconds
            )
        ),
        live_backoff_max_seconds=float(
            os.environ.get("MARKETTWIN_LIVE_BACKOFF_MAX_SECONDS", defaults.live_backoff_max_seconds)
        ),
        live_stale_after_seconds=float(
            os.environ.get("MARKETTWIN_LIVE_STALE_AFTER_SECONDS", defaults.live_stale_after_seconds)
        ),
        live_recording_max_bytes=int(
            os.environ.get("MARKETTWIN_LIVE_RECORDING_MAX_BYTES", defaults.live_recording_max_bytes)
        ),
        live_recording_max_seconds=float(
            os.environ.get(
                "MARKETTWIN_LIVE_RECORDING_MAX_SECONDS", defaults.live_recording_max_seconds
            )
        ),
        live_control_token=os.environ.get("MARKETTWIN_LIVE_CONTROL_TOKEN", ""),
        cors_origins=tuple(origins.split(",")) if origins else defaults.cors_origins,
    )
