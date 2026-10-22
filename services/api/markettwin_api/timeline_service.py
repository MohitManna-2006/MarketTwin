"""Timeline forks and the AI desk (ADR 0012).

A fork is an ordinary run whose config the core builds from a finished parent and one
stage; the core then proves the shared prefix and reports where the futures part. An AI
desk is an ordinary run in which one agent's directives come from a model. The decision
ledger forks a desk run at each decision against the directive it superseded. Nothing in
this module computes a price, a fill, or a P&L figure: every value shown comes from the
core's fork report.
"""

from __future__ import annotations

import json
import secrets
import threading
from concurrent.futures import ThreadPoolExecutor
from typing import Any

import markettwin

from markettwin_api.ai_provider import ProviderError
from markettwin_api.registry import Row, now
from markettwin_api.service import Research, ServiceError

Json = dict[str, Any]

LABEL = "ONE SAMPLE PATH"
DESK_LABEL = "AI DESK (SYNTHETIC MARKET)"
MAX_TIMELINE = 500
DEFAULT_MANDATE = (
    "Make markets in this name. Keep inventory within your position cap, and protect "
    "the desk when the market becomes one-sided or unstable."
)


def _token() -> str:
    return secrets.token_hex(4)


def _same(left: Json, right: Json) -> bool:
    """Whether two directives would trade the same way; the rationale is only words."""
    keys = ("mode", "half_spread_ticks", "skew_ticks", "quote_size", "levels", "max_position")
    return all(left[key] == right[key] for key in keys)


class Timelines:
    def __init__(self, research: Research) -> None:
        self.research = research
        self._ledgers: dict[str, threading.Thread] = {}
        self._lock = threading.Lock()

    # ---- forks --------------------------------------------------------------------------

    def _completed(self, run_id: str) -> Row:
        run = self.research.require_run(run_id)
        if run["status"] != "completed":
            raise ServiceError(
                409, "RunNotCompleted", f"run {run_id} is {run['status']}; fork a completed run"
            )
        return run

    def _fork_config(self, parent: Row, stage: Json) -> Json:
        name = f"fork-{_token()}"
        try:
            config, _ = markettwin.fork_config(self.research.run_dir(parent["id"]), stage, name)
        except markettwin.NativeRunError as error:
            raise ServiceError(
                422,
                "InvalidFork",
                str(error),
                [{"source": "engine", "code": error.code, "path": "", "message": str(error)}],
            ) from error
        return config

    def _desk_of(self, parent: Row, stage: Json) -> Json | None:
        """Where a fork's desk decisions come from: a live fork asks the parent's source."""
        desk = parent.get("desk")
        overridden = any(
            item["policy"]["kind"] == "ModelDirected" for item in stage.get("policy_overrides", [])
        )
        if desk is None and not overridden:
            return None
        source = (desk or {}).get("source", "fixture")
        live = stage.get("decisions") == "live" or overridden
        return {
            "source": source if live else "recorded",
            "model": (desk or {}).get("model"),
            "agents": (desk or {}).get("agents", []),
            "mandate": (desk or {}).get("mandate"),
        }

    def create_fork(self, parent_id: str, stage: Json, title: str | None = None) -> Row:
        parent = self._completed(parent_id)
        config = self._fork_config(parent, stage)
        desk = self._desk_of(parent, stage)
        if desk is not None and desk["source"] == "model":
            self._require_model()
        return self.research.create_run(
            config,
            scenario_key=parent["scenario_key"],
            source_kind="fork",
            extra={
                "parent_run_id": parent_id,
                "fork_at_ns": stage["at_ns"],
                "fork_json": {"title": title, "stage": stage},
                "desk_json": desk,
            },
        )

    def divergence(self, run_id: str) -> Json:
        run = self.research.require_run(run_id)
        if not run.get("parent_run_id"):
            raise ServiceError(404, "NotAFork", f"run {run_id} is not a fork")
        path = self.research.fork_report_path(run_id)
        if not path.is_file():
            raise ServiceError(
                409, "ForkNotVerified", f"run {run_id} is {run['status']} and has no fork report"
            )
        result: Json = json.loads(path.read_text())
        return {"run_id": run_id, "parent_run_id": run["parent_run_id"], **result}

    def _node(self, row: Row) -> Json:
        fork = row.get("fork") or {}
        node: Json = {
            "id": row["id"],
            "name": row["name"],
            "status": row["status"],
            "source_kind": row["source_kind"],
            "parent_run_id": row.get("parent_run_id"),
            "fork_at_ns": row.get("fork_at_ns"),
            "fork_status": row.get("fork_status"),
            "title": fork.get("title"),
            "stage": fork.get("stage"),
            "desk": row.get("desk"),
            "created_at": row["created_at"],
            "horizon_ns": row["horizon_ns"],
            "divergence_ns": None,
            "prefix_records": None,
        }
        if row.get("fork_status") == "verified":
            report = json.loads(self.research.fork_report_path(row["id"]).read_text())["report"]
            first = report["divergence"]["first"]
            node["divergence_ns"] = first["timestamp_ns"] if first else None
            node["prefix_records"] = report["prefix"]["records"]
        return node

    def timeline(self, run_id: str) -> Json:
        """The tree of timelines the run belongs to, from its root, without ledger forks."""
        root = self.research.require_run(run_id)
        seen = {root["id"]}
        while root.get("parent_run_id"):
            parent = self.research.registry.run(root["parent_run_id"])
            if parent is None or parent["id"] in seen:
                break
            seen.add(parent["id"])
            root = parent
        nodes = [self._node(root)]
        frontier = [root["id"]]
        while frontier and len(nodes) < MAX_TIMELINE:
            children = [
                row
                for row in self.research.registry.child_runs(frontier)
                if row["source_kind"] != "ledger"
            ]
            nodes.extend(self._node(row) for row in children)
            frontier = [row["id"] for row in children]
        return {"root_id": root["id"], "focus_id": run_id, "label": LABEL, "nodes": nodes}

    # ---- the AI desk --------------------------------------------------------------------

    def _require_model(self) -> None:
        settings = self.research.settings
        if not settings.ai_model or not settings.ai_api_key:
            raise ServiceError(
                422,
                "ProviderNotConfigured",
                "set MARKETTWIN_AI_MODEL and MARKETTWIN_AI_API_KEY, or use the fixture source",
            )

    def desk_config(self, request: Json) -> tuple[Json, Json]:
        """A scenario with one agent handed to the AI desk, and the desk's details."""
        scenario = self.research.scenarios.get(request["scenario"])
        if scenario is None:
            raise ServiceError(
                422, "UnknownScenario", f"no scenario is named {request['scenario']}"
            )
        config = json.loads(json.dumps(scenario.config))
        agents = (config.get("scenario") or {}).get("agents", [])
        agent = next((item for item in agents if item["id"] == request["agent_id"]), None)
        if agent is None:
            raise ServiceError(
                422,
                "UnknownAgent",
                f"scenario {request['scenario']} has no agent {request['agent_id']}",
            )
        policy = agent["policy"]
        initial = request.get("initial") or {
            "mode": "quote",
            "half_spread_ticks": policy.get("half_spread_ticks", 2),
            "skew_ticks": 0,
            "quote_size": min(policy.get("quote_size", 100), agent["limits"]["max_order_quantity"]),
            "levels": min(policy.get("levels", 1), agent["limits"]["max_open_orders"] // 2),
            "max_position": agent["limits"]["max_position"],
            "rationale": "initial directive",
        }
        agent["policy"] = {
            "kind": "ModelDirected",
            "initial": initial,
            "decision_interval_ns": request["decision_interval_ns"],
            "max_half_spread_ticks": request["max_half_spread_ticks"],
            "max_skew_ticks": request["max_skew_ticks"],
            "mandate": request.get("mandate") or DEFAULT_MANDATE,
        }
        config["schema_version"] = 4
        config["seed"] = request["seed"]
        config["run_id"] = f"desk-{_token()}"
        if request.get("horizon_ns") is not None:
            config["horizon_ns"] = request["horizon_ns"]
        config.setdefault(
            "accounting",
            {
                "fees": {"maker_ppm": 0, "taker_ppm": 0},
                "default_initial_cash_minor": 0,
                "accounts": [],
            },
        )
        config["forks"] = []
        decisions = config["horizon_ns"] // request["decision_interval_ns"]
        settings = self.research.settings
        if decisions > settings.desk_max_decisions:
            raise ServiceError(
                422,
                "DeskBudget",
                f"{decisions} decisions exceed this service's "
                f"{settings.desk_max_decisions} per run",
            )
        model = settings.ai_model if request["source"] == "model" else "fixture"
        desk = {
            "source": request["source"],
            "model": model,
            "agents": [request["agent_id"]],
            "mandate": agent["policy"]["mandate"],
            "decisions": decisions,
            "label": DESK_LABEL,
        }
        return config, desk

    def estimate(self, request: Json) -> Json:
        _, desk = self.desk_config(request)
        configured = bool(self.research.settings.ai_model and self.research.settings.ai_api_key)
        return {
            "model": desk["model"],
            "source": desk["source"],
            "decisions": desk["decisions"],
            "model_calls": desk["decisions"] if desk["source"] == "model" else 0,
            "model_configured": configured,
        }

    def create_desk(self, request: Json) -> Row:
        config, desk = self.desk_config(request)
        if desk["source"] == "model":
            self._require_model()
            if request.get("approved_calls") != desk["decisions"]:
                raise ServiceError(
                    409,
                    "ApprovalRequired",
                    f"this desk asks the model {desk['decisions']} times; "
                    "approve exactly that many calls",
                    {"model_calls": desk["decisions"]},
                )
        return self.research.create_run(
            config, scenario_key=request["scenario"], extra={"desk_json": desk}
        )

    def decisions(self, run_id: str) -> Json:
        run = self._completed(run_id)
        records: list[Json] = []
        with (self.research.run_dir(run_id) / "trace.jsonl").open("rb") as trace:
            for line in trace:
                if b'"kind":"Decision"' not in line:
                    continue
                record = json.loads(line)
                if record["kind"] != "Decision":
                    continue
                payload = record["payload"]
                observation = payload["observation"]
                mids = observation["interval"]["mid_milliticks"]
                records.append(
                    {
                        "at_ns": record["timestamp_ns"],
                        "sequence": record["sequence"],
                        "agent_id": payload["agent_id"],
                        "index": payload["index"],
                        "source": payload["source"],
                        "model": payload["model"],
                        "error_code": payload["error_code"],
                        "input_tokens": payload["input_tokens"],
                        "output_tokens": payload["output_tokens"],
                        "directive": payload["directive"],
                        "position": observation["own"]["position"],
                        "mid_milliticks": mids["last"] if mids else None,
                        "observation": observation,
                    }
                )
        return {
            "run_id": run_id,
            "label": DESK_LABEL,
            "desk": run.get("desk"),
            "decisions": records,
        }

    # ---- the decision ledger ------------------------------------------------------------

    def ledger(self, run_id: str) -> Json:
        run = self.research.require_run(run_id)
        desk = run.get("desk") or {}
        ledger = desk.get("ledger")
        return {"run_id": run_id, "label": LABEL, "ledger": ledger}

    def start_ledger(self, run_id: str) -> Json:
        run = self._completed(run_id)
        desk = run.get("desk")
        if desk is None:
            raise ServiceError(409, "NotADesk", f"run {run_id} has no AI desk")
        current = desk.get("ledger") or {}
        if current.get("status") in ("pending", "running"):
            return self.ledger(run_id)
        listed = self.decisions(run_id)["decisions"]
        initial = {
            agent["id"]: agent["policy"]["initial"]
            for agent in run["config"]["scenario"]["agents"]
            if agent["policy"]["kind"] == "ModelDirected"
        }
        entries: list[Json] = []
        by_agent: dict[int, list[Json]] = {}
        for item in listed:
            by_agent.setdefault(item["agent_id"], []).append(item)
        for agent, items in by_agent.items():
            previous = initial.get(agent)
            for position, item in enumerate(items):
                end = items[position + 1]["at_ns"] if position + 1 < len(items) else None
                changed = previous is not None and not _same(previous, item["directive"])
                # A decision at the horizon has no interval left to be worth anything.
                final = item["at_ns"] >= run["horizon_ns"]
                status = "at_horizon" if final else "pending" if changed else "unchanged"
                entries.append(
                    {
                        "agent_id": agent,
                        "index": item["index"],
                        "at_ns": item["at_ns"],
                        "end_ns": end,
                        "from": previous,
                        "to": item["directive"],
                        "status": status,
                        "fork_run_id": None,
                        "value_minor": None if status == "pending" else 0,
                        "error": None,
                    }
                )
                previous = item["directive"]
        hold: Json | None = None
        first = next(iter(listed), None)
        if first is not None and first["agent_id"] in initial:
            hold = {
                "agent_id": first["agent_id"],
                "at_ns": first["at_ns"],
                "status": "pending",
                "fork_run_id": None,
                "value_minor": None,
                "error": None,
            }
        ledger = {
            "status": "pending",
            "created_at": now(),
            "entries": entries,
            "versus_hold": hold,
            "initial": {str(key): value for key, value in initial.items()},
        }
        self._save(run_id, ledger)
        thread = threading.Thread(
            target=self._run_ledger, args=(run_id,), name=f"ledger-{run_id}", daemon=True
        )
        with self._lock:
            self._ledgers[run_id] = thread
        thread.start()
        return self.ledger(run_id)

    def _save(self, run_id: str, ledger: Json) -> None:
        with self._lock:
            run = self.research.require_run(run_id)
            self.research.registry.update_run(run_id, desk_json={**run["desk"], "ledger": ledger})

    def _ledger_fork(self, parent: Row, stage: Json) -> tuple[str, Json | None, str | None]:
        """Runs one ledger fork on this thread; returns its ID, report, and any error."""
        try:
            config = self._fork_config(parent, stage)
            row = self.research.create_run(
                config,
                scenario_key=parent["scenario_key"],
                source_kind="ledger",
                start=False,
                extra={
                    "parent_run_id": parent["id"],
                    "fork_at_ns": stage["at_ns"],
                    "fork_json": {"title": "decision ledger", "stage": stage},
                    "desk_json": {
                        **(parent.get("desk") or {}),
                        "source": "recorded",
                        "ledger": None,
                    },
                },
            )
        except ServiceError as error:
            return "", None, error.message
        self.research.execute_run(row["id"])
        done = self.research.require_run(row["id"])
        if done["status"] != "completed" or done.get("fork_status") != "verified":
            return row["id"], None, done.get("error_message") or "the fork did not verify"
        report: Json = json.loads(self.research.fork_report_path(row["id"]).read_text())["report"]
        return row["id"], report, None

    @staticmethod
    def _value(report: Json, agent: int) -> int | None:
        """The parent's equity minus the fork's at the end of their shared window: what the
        parent's choice was worth against the alternative, on this path."""
        window = report.get("window")
        if not window:
            return None
        for item in window["agents"]:
            if item["agent_id"] == agent:
                difference = item["values"]["equity_minor"]["difference"]
                return None if difference is None else -int(difference)
        return None

    def _run_ledger(self, run_id: str) -> None:
        parent = self.research.require_run(run_id)
        ledger = parent["desk"]["ledger"]
        ledger["status"] = "running"
        self._save(run_id, ledger)
        lock = threading.Lock()

        def run_entry(entry: Json) -> None:
            stage: Json = {
                "at_ns": entry["at_ns"],
                "decisions": "parent",
                "directive_overrides": [
                    {"agent_id": entry["agent_id"], "directive": entry["from"]}
                ],
            }
            if entry["end_ns"] is not None:
                stage["horizon_ns"] = entry["end_ns"]
            fork_id, report, error = self._ledger_fork(parent, stage)
            with lock:
                entry["fork_run_id"] = fork_id or None
                if report is None:
                    entry.update(status="failed", error=error)
                else:
                    entry.update(
                        status="completed", value_minor=self._value(report, entry["agent_id"])
                    )
                self._save(run_id, ledger)

        def run_hold(entry: Json) -> None:
            initial = ledger["initial"][str(entry["agent_id"])]
            stage = {
                "at_ns": entry["at_ns"],
                "decisions": "hold",
                "directive_overrides": [{"agent_id": entry["agent_id"], "directive": initial}],
            }
            fork_id, report, error = self._ledger_fork(parent, stage)
            with lock:
                entry["fork_run_id"] = fork_id or None
                if report is None:
                    entry.update(status="failed", error=error)
                else:
                    entry.update(
                        status="completed", value_minor=self._value(report, entry["agent_id"])
                    )
                self._save(run_id, ledger)

        pending = [entry for entry in ledger["entries"] if entry["status"] == "pending"]
        workers = max(1, self.research.settings.max_concurrent_jobs)
        try:
            with ThreadPoolExecutor(max_workers=workers, thread_name_prefix="ledger") as pool:
                jobs = [pool.submit(run_entry, entry) for entry in pending]
                if ledger["versus_hold"] is not None:
                    jobs.append(pool.submit(run_hold, ledger["versus_hold"]))
                for job in jobs:
                    job.result()
            failed = any(entry["status"] == "failed" for entry in ledger["entries"])
            ledger["status"] = "partially_completed" if failed else "completed"
        except Exception as error:
            ledger["status"] = "failed"
            ledger["error"] = str(error)
        ledger["finished_at"] = now()
        self._save(run_id, ledger)

    # ---- across seeds -------------------------------------------------------------------

    def repeat(self, run_id: str, seeds: int, first_seed: int) -> Row:
        """The same question over many seeds, as an ordinary paired experiment: a desk
        against the baseline maker it replaced, or a fork against its parent."""
        run = self._completed(run_id)
        settings = self.research.settings
        config = run["config"]
        desk = run.get("desk")
        if run.get("parent_run_id"):
            parent = self.research.require_run(run["parent_run_id"])
            base = parent["config"]
            if base.get("forks") or (desk is not None and desk.get("source") != "recorded"):
                raise ServiceError(
                    422,
                    "UnsupportedRepeat",
                    "repeat over seeds forks a root run without an AI desk",
                )
            stage = dict((run.get("fork") or {})["stage"])
            stage.setdefault("add_shocks", [])
            stage.setdefault("remove_shocks", [])
            stage.setdefault("policy_overrides", [])
            stage.setdefault("horizon_ns", None)
            stage.setdefault("decisions", "hold")
            stage.setdefault("directive_overrides", [])
            variants = [
                {"name": "parent", "policy_overrides": [], "fork": None},
                {"name": "fork", "policy_overrides": [], "fork": {"parent": "parent", **stage}},
            ]
            schema = 2
            overrides: list[Json] = stage["policy_overrides"]
            subject = overrides[0]["agent_id"] if overrides else base["exchange"]["agents"][0]
            comparison = {"baseline": "parent", "candidate": "fork", "subject_agent_id": subject}
            desk_info = None
        elif desk is not None:
            if seeds > settings.desk_max_seeds:
                raise ServiceError(
                    422,
                    "DeskBudget",
                    f"this service repeats a desk over at most {settings.desk_max_seeds} seeds",
                )
            if desk["source"] == "model":
                self._require_model()
            base = config
            agent_id = desk["agents"][0]
            agent = next(item for item in base["scenario"]["agents"] if item["id"] == agent_id)
            initial = agent["policy"]["initial"]
            baseline = {
                "kind": "MarketMaker",
                "half_spread_ticks": initial["half_spread_ticks"],
                "quote_size": initial["quote_size"],
                "levels": initial["levels"],
                "level_spacing_ticks": 1,
            }
            variants = [
                {
                    "name": "baseline",
                    "policy_overrides": [{"agent_id": agent_id, "policy": baseline}],
                },
                {"name": "desk", "policy_overrides": []},
            ]
            schema = 1
            comparison = {"baseline": "baseline", "candidate": "desk", "subject_agent_id": agent_id}
            desk_info = {
                "source": desk["source"],
                "model": desk.get("model"),
                "agents": desk["agents"],
                "model_calls": seeds * int(desk.get("decisions") or 0)
                if desk["source"] == "model"
                else 0,
            }
        else:
            raise ServiceError(422, "UnsupportedRepeat", "repeat a fork or an AI desk run")
        experiment = {
            "schema_version": schema,
            "experiment_id": f"repeat-{_token()}",
            "base_config": base,
            "horizon_ns": None,
            "seeds": {"first": first_seed, "count": seeds},
            "variants": variants,
            "comparison": comparison,
            "bootstrap": {"resamples": 1000, "seed": 1},
            "artifacts": "None",
            "limits": {"max_runs": 2 * seeds, "max_output_bytes": 0},
        }
        try:
            return self.research.create_experiment(
                experiment, scenario_key=run["scenario_key"], desk=desk_info
            )
        except ProviderError as error:
            raise ServiceError(422, error.code, str(error)) from error
