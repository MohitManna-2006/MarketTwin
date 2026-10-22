"""Produce the Phase 6 evidence (ADR 0012): an AI desk run, its decision ledger, one fork,
and the desk against the baseline maker over paired seeds.

    uv run python scripts/desk_evidence.py --source fixture \
        --export ../../docs/experiments/phase6-desk

With ``--source model`` the configured OpenAI-compatible model decides (set
MARKETTWIN_AI_MODEL and MARKETTWIN_AI_API_KEY) and ``--approve-calls`` must equal the number
of model calls the run will make, which the script prints first. Everything runs in a
separate workspace (``--workspace``), so the terminal's own runs are untouched.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import sys
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any

from markettwin_api.registry import Row
from markettwin_api.service import Research, ServiceError
from markettwin_api.settings import load_settings
from markettwin_api.timeline_service import Timelines

SECOND = 1_000_000_000
Json = dict[str, Any]


def settle(read: Callable[[], Row], label: str, done: Callable[[Row], bool]) -> Row:
    last = ""
    while True:
        row = read()
        line = f"{label}: {row.get('status')}"
        if line != last:
            print(line, flush=True)
            last = line
        if done(row):
            return row
        time.sleep(0.5)


def finished(row: Row) -> bool:
    return row["status"] not in ("pending", "running")


def write(directory: Path, name: str, value: Any) -> None:
    (directory / name).write_text(json.dumps(value, indent=1, sort_keys=True) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source", choices=("fixture", "model"), default="fixture")
    parser.add_argument("--approve-calls", type=int, default=None)
    parser.add_argument("--seeds", type=int, default=20)
    parser.add_argument("--workspace", type=Path, default=Path("../../data/phase6-evidence"))
    parser.add_argument("--export", type=Path, required=True)
    arguments = parser.parse_args()

    settings = dataclasses.replace(load_settings(), workspace=arguments.workspace.resolve())
    research = Research(settings)
    timelines = Timelines(research)
    request: Json = {
        "scenario": "latency_shock",
        "agent_id": 3,
        "seed": 42,
        "horizon_ns": None,
        "mandate": "Make markets in this name. Keep inventory within 2000 shares, and protect "
        "the desk when the market becomes one-sided or unstable.",
        "decision_interval_ns": 60 * SECOND,
        "max_half_spread_ticks": 20,
        "max_skew_ticks": 10,
        "initial": {
            "mode": "quote",
            "half_spread_ticks": 2,
            "skew_ticks": 0,
            "quote_size": 100,
            "levels": 3,
            # The agent's own limit, so the desk and the baseline maker it is compared
            # with over seeds work under the same position cap.
            "max_position": 2000,
            "rationale": "initial directive",
        },
        "source": arguments.source,
        "approved_calls": arguments.approve_calls,
    }
    try:
        estimate = timelines.estimate(request)
        print("desk:", json.dumps(estimate))
        print(
            "across seeds: about",
            estimate["model_calls"] * arguments.seeds,
            "more model calls",
        )
        if arguments.source == "model" and arguments.approve_calls != estimate["model_calls"]:
            print("pass --approve-calls", estimate["model_calls"], "to run the model desk")
            return 2
        run = timelines.create_desk(request)
        run = settle(lambda: research.require_run(run["id"]), f"desk {run['id']}", finished)
        if run["status"] != "completed":
            print(f"desk failed: {run['error_code']}: {run['error_message']}")
            return 1

        timelines.start_ledger(run["id"])
        ledger = settle(
            lambda: {"status": (timelines.ledger(run["id"])["ledger"] or {}).get("status")},
            "decision ledger",
            lambda row: row["status"] in ("completed", "partially_completed", "failed"),
        )
        stress = {
            "at_ns": 1_500 * SECOND,
            "decisions": "live",
            "add_shocks": [
                {"kind": "QuoteWithdrawalStress", "at_ns": 1_500 * SECOND, "agents": [1, 2]}
            ],
        }
        fork = timelines.create_fork(run["id"], stress, "makers 1 and 2 pull quotes at 25 min")
        fork = settle(lambda: research.require_run(fork["id"]), f"fork {fork['id']}", finished)
        experiment = timelines.repeat(run["id"], arguments.seeds, 1)
        experiment = settle(
            lambda: research.require_experiment(experiment["id"]),
            f"experiment {experiment['id']}",
            finished,
        )

        export = arguments.export
        export.mkdir(parents=True, exist_ok=True)
        decisions = timelines.decisions(run["id"])
        for item in decisions["decisions"]:
            item.pop("observation")
        desk_run = research.require_run(run["id"])
        write(
            export,
            "desk_run.json",
            {
                "run_id": run["id"],
                "config": desk_run["config"],
                "summary": desk_run["summary"],
                "desk": {
                    key: value for key, value in (desk_run["desk"] or {}).items() if key != "ledger"
                },
            },
        )
        write(export, "decisions.json", decisions)
        write(export, "decision_ledger.json", timelines.ledger(run["id"]))
        write(export, "fork_report.json", timelines.divergence(fork["id"]))
        write(export, "experiment.json", research.experiment_document(experiment["id"]))
        print("ledger:", ledger["status"], "experiment:", experiment["status"])
        print("exported to", export)
        return 0
    except ServiceError as error:
        print(f"{error.code}: {error.message}", file=sys.stderr)
        return 1
    finally:
        research.close()


if __name__ == "__main__":
    sys.exit(main())
