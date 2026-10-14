"""The scenario catalogue: the run configs shipped in ``configs/``.

Only the files named here are offered. Titles and descriptions are editorial; every
number shown for a scenario is read from its config after the core has validated it.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import markettwin

from markettwin_api.config_models import (
    POLICY_DESCRIPTIONS,
    POLICY_MODELS,
    SHOCK_DESCRIPTIONS,
    SHOCK_MODELS,
    AccountingModel,
    AccountModel,
    FeesModel,
    FundamentalModel,
    LatencyModel,
    LimitsModel,
    RunConfigBase,
    SamplingModel,
    ScenarioModel,
    Strict,
    WakeupModel,
)

Json = dict[str, Any]

_CATALOGUE: tuple[tuple[str, str, str, str], ...] = (
    (
        "baseline",
        "Baseline",
        "scenario",
        "Two baseline makers, one inventory-aware maker, two value traders, two momentum "
        "traders, and four noise traders over one simulated hour. No shock.",
    ),
    (
        "fundamental_shock",
        "Fundamental jump",
        "scenario",
        "The baseline with the latent fundamental value dropping 150 ticks at 30 minutes.",
    ),
    (
        "liquidity_shock",
        "Liquidity withdrawal",
        "scenario",
        "The baseline with both baseline makers withdrawn from minute 20 to minute 30.",
    ),
    (
        "volatility_shock",
        "Volatility regime",
        "scenario",
        "The baseline with fundamental step volatility quadrupled from minute 20 to minute 40.",
    ),
    (
        "order_flow_shock",
        "Order-flow imbalance",
        "scenario",
        "The baseline with the four noise traders buying 80% of the time, sending 60% market "
        "orders, at twice their rate from minute 20 to minute 30.",
    ),
    (
        "latency_shock",
        "Latency degradation",
        "scenario",
        "The baseline with the three makers' orders and cancels slowed by 50 ms, reports by "
        "20 ms, and market data by 50 ms from minute 20 to minute 30.",
    ),
    (
        "quote_stress_shock",
        "Quote withdrawal stress",
        "scenario",
        "The baseline with all three makers canceling every working quote at minute 30.",
    ),
    (
        "baseline_accounting",
        "Baseline with fees and credit limits",
        "scenario",
        "The baseline with a 30 ppm taker fee, a 20 ppm maker rebate, opening cash, and a zero "
        "credit limit on the three makers.",
    ),
    (
        "phase1b_demo",
        "Scripted exchange demo",
        "scripted",
        "A scripted run with no agents: every order, cancel, snapshot, and stop comes from "
        "the config. It has no fundamental value, no sampling schedule, and no policies.",
    ),
)


@dataclass(frozen=True)
class Scenario:
    key: str
    title: str
    kind: str
    description: str
    config: Json
    config_digest: str

    def summary(self) -> Json:
        scenario = self.config.get("scenario") or {}
        agents = scenario.get("agents", [])
        policies: dict[str, int] = {}
        for agent in agents:
            kind = agent["policy"]["kind"]
            policies[kind] = policies.get(kind, 0) + 1
        return {
            "key": self.key,
            "title": self.title,
            "kind": self.kind,
            "description": self.description,
            "schema_version": self.config["schema_version"],
            "seed": self.config["seed"],
            "horizon_ns": self.config["horizon_ns"],
            "tick_size_minor": self.config["tick_size_minor"],
            "agent_count": len(agents) if scenario else len(self.config["exchange"]["agents"]),
            "policies": policies,
            "shocks": [shock["kind"] for shock in scenario.get("shocks", [])],
            "has_accounting": "accounting" in self.config,
            "config_digest": self.config_digest,
        }


class ScenarioCatalogue:
    def __init__(self, directory: Path) -> None:
        self._scenarios: dict[str, Scenario] = {}
        for key, title, kind, description in _CATALOGUE:
            path = directory / f"{key}.json"
            if not path.is_file():
                continue
            text = path.read_text()
            _, digest = markettwin.canonicalize_config(text)
            self._scenarios[key] = Scenario(key, title, kind, description, json.loads(text), digest)

    def list(self) -> list[Scenario]:
        return list(self._scenarios.values())

    def get(self, key: str) -> Scenario | None:
        return self._scenarios.get(key)

    def key_for_digest(self, digest: str) -> str | None:
        for scenario in self._scenarios.values():
            if scenario.config_digest == digest:
                return scenario.key
        return None

    def policy_templates(self) -> dict[str, Json]:
        """One complete agent per policy, taken from the shipped baseline scenario."""
        templates: dict[str, Json] = {}
        baseline = self.get("baseline")
        if baseline is None:
            return templates
        for agent in baseline.config["scenario"]["agents"]:
            templates.setdefault(agent["policy"]["kind"], agent)
        return templates


def _fields(model: type[Strict]) -> list[Json]:
    """Name, unit, bounds, and meaning of every integer or enumerated field of a model."""
    schema = model.model_json_schema()
    result = []
    for name, spec in schema.get("properties", {}).items():
        if name == "kind":
            continue
        options = spec.get("enum")
        nullable = False
        if "anyOf" in spec:
            nullable = any(item.get("type") == "null" for item in spec["anyOf"])
            typed = [item for item in spec["anyOf"] if item.get("type") != "null"]
            spec = {**spec, **(typed[0] if typed else {})}
        if spec.get("type") not in ("integer", "string") and options is None:
            continue
        result.append(
            {
                "name": name,
                "unit": spec.get("unit", ""),
                "description": spec.get("description", ""),
                "minimum": spec.get("minimum"),
                "maximum": spec.get("maximum"),
                "options": options,
                "nullable": nullable,
            }
        )
    return result


def field_catalogue(catalogue: ScenarioCatalogue) -> Json:
    """What the Simulation Lab may offer a control for: exactly the native config schema."""
    templates = catalogue.policy_templates()
    return {
        "run": _fields(RunConfigBase),
        "scenario": _fields(ScenarioModel),
        "fundamental": _fields(FundamentalModel),
        "sampling": _fields(SamplingModel),
        "wakeup": _fields(WakeupModel),
        "latency": _fields(LatencyModel),
        "limits": _fields(LimitsModel),
        "fees": _fields(FeesModel),
        "accounting": _fields(AccountingModel),
        "account": _fields(AccountModel),
        "policies": [
            {
                "kind": kind,
                "description": POLICY_DESCRIPTIONS[kind],
                "fields": _fields(model),
                "template": templates.get(kind),
            }
            for kind, model in POLICY_MODELS.items()
        ],
        "shocks": [
            {"kind": kind, "description": SHOCK_DESCRIPTIONS[kind], "fields": _fields(model)}
            for kind, model in SHOCK_MODELS.items()
        ],
    }
