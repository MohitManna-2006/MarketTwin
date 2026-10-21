"""One calibration from admitted sessions to a published artifact (ADR 0011).

Python decides only what to run and in which order: which sessions are admitted, which
candidates are drawn, which seeds each stage uses, and which candidate is kept. Every
fact, target, distance, and score comes from the native core. Runs are independent and
execute in parallel, but results are ordered by (candidate, seed) before anything reads
them, so the artifact does not depend on scheduling.
"""

from __future__ import annotations

import hashlib
import json
import shutil
import threading
from collections.abc import Callable
from concurrent.futures import FIRST_COMPLETED, Future, ThreadPoolExecutor, wait
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import markettwin

from markettwin_api.calibration_spec import (
    ACCEPT_DISTANCE_PPM,
    ACCEPT_FACTS,
    GATE_TWO_SIDED_PPM,
    GRID_KEYS,
    MINIMUM_ADMITTED,
    ROLES,
    CalibrationSpec,
    candidate_config,
    canonical_json,
    deviations,
    draw_candidates,
    parse_utc,
    spec_digest,
    utc_ns,
)

Json = dict[str, Any]
Progress = Callable[[str, int, int], None]

ARTIFACT_FILES = (
    "spec.json",
    "observed_facts.json",
    "candidates.jsonl",
    "selected_config.json",
    "report.json",
)


class CalibrationError(Exception):
    """The calibration cannot produce a result. ``code`` is stable."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


@dataclass(frozen=True)
class SessionInput:
    id: str
    day: str  # local trading date, ISO
    role: str
    directory: Path
    start: str
    end: str
    start_ns: int
    end_ns: int
    canonical_sha256: str | None
    gaps_over_five_minutes: int


@dataclass
class Cell:
    candidate: int
    seed: int
    facts: Json | None = None
    error: str | None = None
    config_digest: str | None = None


@dataclass
class Context:
    calibration_id: str
    spec: CalibrationSpec
    base: Json
    observation_kind: str
    workers: int
    progress: Progress
    cancelled: Callable[[], bool]
    build: Json = field(default_factory=dict)


def labels(observation_kind: str) -> tuple[str, str]:
    """(label when the fit holds, label for a null result)."""
    if observation_kind == "OBSERVED IEX DATA":
        return "CALIBRATED (FIT TO OBSERVED IEX FACTS)", "CALIBRATION NULL RESULT"
    return (
        "CALIBRATED (FIT TO FABRICATED TEST FACTS)",
        "CALIBRATION NULL RESULT (FABRICATED TEST DATA)",
    )


def _seed_facts(document: Json) -> Json:
    """What a candidates.jsonl line keeps of a facts document: no repeated unit tables."""
    return {key: document[key] for key in ("facts", "counts", "unscored", "run")}


def admit(session: SessionInput) -> tuple[Json, list[str]]:
    """Facts of one session and the reasons it fails the ADR 0011 quality gate."""
    reasons: list[str] = []
    verdict = markettwin.verify_historical(session.directory)
    if not verdict.get("verified"):
        reasons.append("Phase 4A verification failed")
    facts = markettwin.calibration_observed_facts(
        session.directory, session.start_ns, session.end_ns
    )
    two_sided = facts["facts"]["two_sided_share"]
    if two_sided is None or two_sided < GATE_TWO_SIDED_PPM:
        reasons.append("quotes are two-sided for less than 90% of the window")
    undefined = [name for name, value in facts["facts"].items() if value is None]
    if undefined:
        reasons.append("undefined facts: " + ", ".join(sorted(undefined)))
    if session.gaps_over_five_minutes:
        reasons.append("a quote or trade gap exceeds five minutes (halt check)")
    if facts["unscored"]["opening_midpoint_micro_usd"] is None:
        reasons.append("no two-sided quote opens the window")
    return facts, reasons


class Runner:
    """Runs (candidate, seed) cells on a thread pool; the native call releases the GIL."""

    def __init__(self, context: Context, configs: Callable[[int, int], Json]) -> None:
        self.context = context
        self.configs = configs
        self.completed = 0
        self.total = 0
        self.phase = ""
        self._lock = threading.Lock()

    def _one(self, cell: Cell) -> Cell:
        config = self.configs(cell.candidate, cell.seed)
        try:
            document = markettwin.calibration_synthetic_facts(config)
            cell.facts = _seed_facts(document)
            cell.config_digest = document["run"]["config_digest"]
        except markettwin.NativeRunError as error:
            cell.error = getattr(error, "code", "NativeRunError")
        except Exception as error:  # recorded as a failed seed, never dropped
            cell.error = type(error).__name__
        with self._lock:
            self.completed += 1
            done = self.completed
        if done % 25 == 0 or done == self.total:
            self.context.progress(self.phase, done, self.total)
        return cell

    def run(self, phase: str, cells: list[Cell]) -> list[Cell]:
        self.phase = phase
        self.completed = 0
        self.total = len(cells)
        self.context.progress(phase, 0, self.total)
        with ThreadPoolExecutor(
            max_workers=self.context.workers, thread_name_prefix="markettwin-calibration"
        ) as executor:
            pending: set[Future[Cell]] = {executor.submit(self._one, cell) for cell in cells}
            while pending:
                if self.context.cancelled():
                    for future in pending:
                        future.cancel()
                    raise CalibrationError("Canceled", "the calibration was stopped")
                _, pending = wait(pending, timeout=0.5, return_when=FIRST_COMPLETED)
        return sorted(cells, key=lambda cell: (cell.candidate, cell.seed))


def score(cells: list[Cell], target: Json, scale: Json) -> Json:
    failed = [cell for cell in cells if cell.facts is None]
    if failed:
        return {
            "valid": False,
            "invalid_reason": f"seed {failed[0].seed} failed: {failed[0].error}",
            "seeds": len(cells),
            "facts": None,
            "score_ppm": None,
        }
    return markettwin.calibration_score(
        target, scale, [cell.facts for cell in cells if cell.facts is not None]
    )


def _rank_key(entry: Json, field_name: str) -> tuple[int, int]:
    return (entry[field_name]["score_ppm"], entry["candidate"])


def _order_statistic(values: list[int], quantile_percent: int) -> int | None:
    if not values:
        return None
    ordered = sorted(values)
    return ordered[(len(ordered) - 1) * quantile_percent // 100]


def _lower_median(values: list[int]) -> int:
    ordered = sorted(values)
    return ordered[(len(ordered) - 1) // 2]


def run_calibration(context: Context, sessions: list[SessionInput], output: Path) -> Json:
    spec = context.spec
    if output.exists():
        raise CalibrationError("OutputExists", "the calibration directory already exists")

    # 1. Observed facts and the quality gate.
    context.progress("observed", 0, len(sessions))
    observed: list[Json] = []
    for done, session in enumerate(sessions, start=1):
        facts, rejected = admit(session)
        observed.append(
            {
                "session_id": session.id,
                "day": session.day,
                "role": session.role,
                "start": session.start,
                "end": session.end,
                "canonical_sha256": session.canonical_sha256,
                "admitted": not rejected,
                "rejection_reasons": rejected,
                "facts": facts,
            }
        )
        context.progress("observed", done, len(sessions))
    admitted = {
        role: [item["facts"] for item in observed if item["role"] == role and item["admitted"]]
        for role in ROLES
    }
    for role, minimum in MINIMUM_ADMITTED.items():
        if len(admitted[role]) < minimum:
            raise CalibrationError(
                "TooFewSessions",
                f"{len(admitted[role])} {role} sessions passed the quality gate; "
                f"ADR 0011 needs at least {minimum}",
            )
    targets = {role: markettwin.calibration_targets(admitted[role]) for role in ROLES}
    reference = targets["train"]["reference_price_cents"]
    if reference is None:
        raise CalibrationError("NoReferencePrice", "train sessions have no opening midpoint")

    # 2. Candidates: 0 is the base config unchanged, then the stage 1 draws.
    parameters: list[Json | None] = [None, *draw_candidates(spec)]

    def configs(candidate: int, seed: int) -> Json:
        return candidate_config(
            context.base,
            parameters[candidate],
            reference,
            seed,
            f"{context.calibration_id}-c{candidate:04d}-s{seed}",
        )

    runner = Runner(context, configs)
    lines: list[Json] = []

    def stage(name: str, candidates: list[int], seeds: list[int]) -> dict[int, list[Cell]]:
        cells = [Cell(candidate, seed) for candidate in candidates for seed in seeds]
        finished = runner.run(name, cells)
        grouped: dict[int, list[Cell]] = {candidate: [] for candidate in candidates}
        for cell in finished:
            grouped[cell.candidate].append(cell)
        return grouped

    def record(name: str, candidate: int, cells: list[Cell], scores: Json) -> None:
        lines.append(
            {
                "stage": name,
                "candidate": candidate,
                "parameters": parameters[candidate],
                "scores": scores,
                "seeds": [
                    {
                        "seed": cell.seed,
                        "config_digest": cell.config_digest,
                        "facts": cell.facts,
                        "error": cell.error,
                    }
                    for cell in cells
                ],
            }
        )

    # 3. Stage 1: every candidate on the stage 1 seeds, scored against train.
    stage1 = stage("stage1", list(range(len(parameters))), spec.seeds("stage1"))
    stage1_scores: list[Json] = []
    for candidate, cells in stage1.items():
        fit = score(cells, targets["train"], targets["train"])
        stage1_scores.append({"candidate": candidate, "fit": fit})
        record("stage1", candidate, cells, {"fit": fit})
    valid1 = [entry for entry in stage1_scores if entry["fit"]["valid"]]
    finalists = [
        entry["candidate"]
        for entry in sorted(valid1, key=lambda entry: _rank_key(entry, "fit"))[
            : spec.search.stage2_top
        ]
    ]

    # 4. Stage 2: the finalists on fresh seeds; train scores fit, validation selects.
    stage2_scores: list[Json] = []
    if finalists:
        stage2 = stage("stage2", finalists, spec.seeds("stage2"))
        for candidate in finalists:
            cells = stage2[candidate]
            fit = score(cells, targets["train"], targets["train"])
            selection = score(cells, targets["validation"], targets["train"])
            stage2_scores.append({"candidate": candidate, "fit": fit, "validation": selection})
            record("stage2", candidate, cells, {"fit": fit, "validation": selection})
    valid2 = [
        entry for entry in stage2_scores if entry["fit"]["valid"] and entry["validation"]["valid"]
    ]
    selected: int | None = None
    if valid2:
        selected = min(
            valid2,
            key=lambda entry: (
                entry["validation"]["score_ppm"],
                entry["fit"]["score_ppm"],
                entry["candidate"],
            ),
        )["candidate"]

    # 5. Test, once: the selected candidate and the baseline on the test seeds.
    test: Json = {"selected": None, "baseline": None}
    if selected is not None:
        tested = stage("test", sorted({selected, 0}), spec.seeds("test"))
        for candidate, cells in tested.items():
            result = score(cells, targets["test"], targets["train"])
            record("test", candidate, cells, {"test": result})
            if candidate == selected:
                test["selected"] = result
            if candidate == 0:
                test["baseline"] = result

    # 6. Acceptance, fixed in ADR 0011 before any data was imported.
    reasons: list[str] = []
    within = 0
    beats = False
    chosen, baseline = test["selected"], test["baseline"]
    if selected is None:
        reasons.append("no candidate was valid on both train and validation in stage 2")
    elif not chosen["valid"]:
        reasons.append("the selected candidate was invalid on the test seeds")
    else:
        within = sum(
            1
            for fact in chosen["facts"].values()
            if abs(fact["distance_ppm"]) <= ACCEPT_DISTANCE_PPM
        )
        if within < ACCEPT_FACTS:
            reasons.append(f"only {within} of 12 facts are within two scales on test")
        if not baseline["valid"]:
            reasons.append("the baseline has no valid test score to beat")
        elif selected == 0:
            reasons.append("the baseline itself was selected, so it cannot beat itself")
        else:
            beats = chosen["score_ppm"] < baseline["score_ppm"]
            if not beats:
                reasons.append("the selected candidate does not beat the baseline on test")
    holds = not reasons
    fit_label, null_label = labels(context.observation_kind)

    # 7. Report.
    valid_scores = [entry["fit"]["score_ppm"] for entry in valid1]
    sensitivity: Json = {}
    for key in GRID_KEYS:
        by_value: dict[int, list[int]] = {}
        for entry in valid1:
            point = parameters[entry["candidate"]]
            if point is not None:
                by_value.setdefault(point[key], []).append(entry["fit"]["score_ppm"])
        sensitivity[key] = {
            "stage1_median_fit_by_value": [
                {"value": value, "candidates": len(scores), "median_fit_ppm": _lower_median(scores)}
                for value, scores in sorted(by_value.items())
            ],
            "stage2_validation": [
                {
                    "candidate": entry["candidate"],
                    "value": (parameters[entry["candidate"]] or {}).get(key),
                    "validation_ppm": entry["validation"]["score_ppm"],
                }
                for entry in stage2_scores
            ],
        }
    days = sorted(item["day"] for item in observed if item["admitted"])
    statement = (
        f"On the listed facts, this config is within the shown distances of "
        f"{context.observation_kind.lower()} sessions of {spec.symbol} on "
        f"{', '.join(days)}, {spec.window.start_local} to {spec.window.end_local} ET."
    )
    report: Json = {
        "schema_version": 1,
        "calibration_id": context.calibration_id,
        "title": spec.title,
        "symbol": spec.symbol,
        "feed": spec.feed,
        "observation_kind": context.observation_kind,
        "window": spec.window.model_dump(),
        "outcome": "fit_holds" if holds else "null_result",
        "label": fit_label if holds else null_label,
        "null_reasons": reasons,
        "claim": statement
        if holds
        else "The fit did not hold out of sample; no closeness to observed facts is claimed.",
        "not_claimed": [
            "realism of the synthetic market",
            "prices, volatility, or liquidity outside the listed facts",
            "participant latency or market impact",
            "that any strategy result in this config holds in a real market",
        ],
        "acceptance": {
            "distance_ppm_limit": ACCEPT_DISTANCE_PPM,
            "facts_required": ACCEPT_FACTS,
            "facts_within": within,
            "beats_baseline": beats,
        },
        "deviations_from_adr": deviations(spec),
        "sessions": [
            {key: item[key] for key in ("session_id", "day", "role", "admitted")}
            | {"rejection_reasons": item["rejection_reasons"]}
            for item in observed
        ],
        "targets": targets,
        "reference_price_cents": reference,
        "stage1": {
            "candidates": len(stage1_scores),
            "valid": len(valid1),
            "invalid": len(stage1_scores) - len(valid1),
            "fit_ppm_quantiles": {
                f"p{quantile}": _order_statistic(valid_scores, quantile)
                for quantile in (0, 10, 50, 90, 100)
            },
            "fit_ppm": sorted((entry["fit"]["score_ppm"], entry["candidate"]) for entry in valid1),
            "baseline_fit": stage1_scores[0]["fit"],
            "finalists": finalists,
        },
        "stage2": [
            {
                "candidate": entry["candidate"],
                "parameters": parameters[entry["candidate"]],
                "fit": entry["fit"],
                "validation": entry["validation"],
            }
            for entry in stage2_scores
        ],
        "selected": None
        if selected is None
        else {"candidate": selected, "parameters": parameters[selected]},
        "test": test,
        "sensitivity": sensitivity,
        "seeds": {name: spec.seeds(name) for name in ("stage1", "stage2", "test")},
    }

    # 8. Publish: every file in a staging directory, then one rename.
    staging = output.with_name(output.name + ".partial")
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)
    try:
        spec_document = {
            "schema_version": 1,
            "calibration_id": context.calibration_id,
            "spec": spec.model_dump(),
            "spec_sha256": spec_digest(spec),
            "base_config": context.base,
            "observation_kind": context.observation_kind,
            "sessions": [
                {
                    "session_id": session.id,
                    "day": session.day,
                    "role": session.role,
                    "start": session.start,
                    "end": session.end,
                }
                for session in sessions
            ],
            "seeds": report["seeds"],
            "deviations_from_adr": deviations(spec),
        }
        _write_json(staging / "spec.json", spec_document)
        _write_json(staging / "observed_facts.json", {"sessions": observed, "targets": targets})
        with (staging / "candidates.jsonl").open("w", encoding="utf-8") as stream:
            for line in lines:
                stream.write(json.dumps(line, sort_keys=True, separators=(",", ":")) + "\n")
        selected_config = None
        if selected is not None:
            canonical, _ = markettwin.canonicalize_config(
                candidate_config(
                    context.base,
                    parameters[selected],
                    reference,
                    spec.search.test_seed_base,
                    f"calibrated-{spec.symbol.lower()}-{spec.feed.replace('_', '-')}",
                )
            )
            selected_config = json.loads(canonical)
        _write_json(staging / "selected_config.json", selected_config)
        _write_json(staging / "report.json", report)
        manifest = {
            "schema_version": 1,
            "calibration_id": context.calibration_id,
            "engine": context.build,
            "files": {name: _sha256(staging / name) for name in ARTIFACT_FILES},
        }
        _write_json(staging / "manifest.json", manifest)
        staging.rename(output)
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    return report


def _write_json(path: Path, value: Any) -> None:
    path.write_bytes(canonical_json(value) + b"\n")


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(directory: Path, sessions: dict[str, Path], sample: int = 2) -> Json:
    """Re-hash the artifact, recompute observed facts, and re-run a sample of seeds."""
    checks: list[Json] = []
    manifest = json.loads((directory / "manifest.json").read_text())
    for name, digest in manifest["files"].items():
        actual = _sha256(directory / name)
        checks.append({"check": f"sha256 {name}", "ok": actual == digest})
    observed = json.loads((directory / "observed_facts.json").read_text())
    for item in observed["sessions"]:
        path = sessions.get(item["session_id"])
        if path is None:
            checks.append({"check": f"observed {item['session_id']}", "ok": False})
            continue
        facts = markettwin.calibration_observed_facts(
            path, utc_ns(parse_utc(item["start"])), utc_ns(parse_utc(item["end"]))
        )
        same = all(facts[key] == item["facts"][key] for key in ("facts", "counts", "unscored"))
        checks.append({"check": f"observed {item['session_id']}", "ok": same})
    spec_document = json.loads((directory / "spec.json").read_text())
    spec = CalibrationSpec.model_validate(spec_document["spec"])
    report = json.loads((directory / "report.json").read_text())
    reference = report["reference_price_cents"]
    parameters: list[Json | None] = [None, *draw_candidates(spec)]
    rerun = 0
    with (directory / "candidates.jsonl").open() as stream:
        for raw in stream:
            line = json.loads(raw)
            if line["stage"] not in ("stage1", "test") or rerun >= sample:
                continue
            if line["stage"] == "stage1" and line["candidate"] != 0:
                continue
            seed = line["seeds"][0]
            config = candidate_config(
                spec_document["base_config"],
                parameters[line["candidate"]],
                reference,
                seed["seed"],
                f"{report['calibration_id']}-c{line['candidate']:04d}-s{seed['seed']}",
            )
            document = markettwin.calibration_synthetic_facts(config)
            checks.append(
                {
                    "check": f"rerun {line['stage']} candidate {line['candidate']} "
                    f"seed {seed['seed']}",
                    "ok": _seed_facts(document) == seed["facts"],
                }
            )
            rerun += 1
    return {"verified": all(check["ok"] for check in checks), "checks": checks}
