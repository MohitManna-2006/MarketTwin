"""``python -m markettwin_api``: serve the research API on the loopback interface."""

from __future__ import annotations

import argparse

import uvicorn


def main() -> None:
    parser = argparse.ArgumentParser(description="MarketTwin research API")
    parser.add_argument("--host", default="127.0.0.1", help="interface to bind (default: loopback)")
    parser.add_argument("--port", type=int, default=8000)
    arguments = parser.parse_args()
    uvicorn.run(
        "markettwin_api.app:create_app", factory=True, host=arguments.host, port=arguments.port
    )


if __name__ == "__main__":
    main()
