"""Durable supervised research workflow over existing native experiment jobs."""

from __future__ import annotations

import hashlib
import json
import re
import secrets
import threading
import time
from decimal import Decimal
from typing import Any

from markettwin_api.ai_models import (
    NATIVE_EXPERIMENT_METRICS,
    Claim,
    Intent,
    Proposal,
    ResearchPlan,
)
from markettwin_api.ai_provider import (
    FixtureProvider,
    ModelProvider,
    OpenAICompatibleProvider,
    ProviderError,
)
from markettwin_api.ai_tools import ResearchTools
from markettwin_api.registry import Row, now
from markettwin_api.service import Research, ServiceError

Json = dict[str, Any]
METRICS = frozenset(NATIVE_EXPERIMENT_METRICS)
TERMINAL = {"REPORT_READY", "REJECTED", "CANCELLED", "FAILED", "PARTIAL", "UNSUPPORTED"}


def _digest(value: Json) -> str:
    return hashlib.sha256(
        json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()


def _no_numbers(value: str) -> bool:
    return re.search(r"\d", value) is None


class AIResearch:
    def __init__(self, research: Research, provider: ModelProvider | None = None) -> None:
        self.research = research
        self.registry = research.registry
        self.provider = provider or (
            FixtureProvider()
            if research.settings.ai_provider == "fixture"
            else OpenAICompatibleProvider(research.settings)
        )
        self.tools = ResearchTools(research)
        self._threads: list[threading.Thread] = []
        self._closed = threading.Event()
        self._recover()

    def close(self) -> None:
        self._closed.set()
        for thread in self._threads:
            thread.join(timeout=2)

    def _recover(self) -> None:
        # Native jobs are marked interrupted by Registry on startup. No job is resumed
        # without its original process. The investigation reflects that terminal state.
        for row in self.registry.investigations(1000, 0):
            if row["status"] == "EXECUTING":
                experiment = self.registry.experiment(row["experiment_id"])
                if experiment and experiment["status"] in ("completed", "partially_completed"):
                    self._monitor(row["id"], row["experiment_id"])
                else:
                    self.registry.transition_investigation(
                        row["id"],
                        ("EXECUTING",),
                        "FAILED",
                        error_code="WorkerInterrupted",
                        error_message="experiment was interrupted",
                    )

    def require(self, investigation_id: str) -> Row:
        row = self.registry.investigation(investigation_id)
        if row is None:
            raise ServiceError(404, "InvestigationNotFound", "no investigation has that ID")
        return row

    def view(self, investigation_id: str) -> Json:
        row = self.require(investigation_id)
        experiment = (
            self.registry.experiment(row["experiment_id"]) if row["experiment_id"] else None
        )
        return {
            **row,
            "experiment_status": experiment["status"] if experiment else None,
            "progress": self.research.experiment_progress(row["experiment_id"])
            if experiment
            else None,
            "events": self.registry.ai_events(investigation_id),
            "messages": self.registry.ai_messages(investigation_id),
        }

    def create(self, question: str, parent_id: str | None = None) -> Json:
        if parent_id:
            self.require(parent_id)
        investigation_id = "inv-" + secrets.token_hex(6)
        provider_name = self.research.settings.ai_provider
        inserted = self.registry.insert_investigation_bounded(
            {
                "id": investigation_id,
                "question": question,
                "status": "CREATED",
                "created_at": now(),
                "updated_at": now(),
                "provider": provider_name,
                "model": self.research.settings.ai_model,
                "parent_id": parent_id,
            },
            self.research.settings.ai_max_investigations,
            self.research.settings.ai_max_concurrent_investigations,
        )
        if not inserted:
            raise ServiceError(429, "InvestigationLimit", "investigation capacity reached")
        self.registry.ai_message(investigation_id, "user", question)
        self.registry.ai_event(investigation_id, "CREATED", "request_received", "ok")
        return self.view(investigation_id)

    def plan(self, investigation_id: str) -> Json:
        row = self.require(investigation_id)
        if row["status"] not in ("CREATED", "FAILED") or row["experiment_id"]:
            raise ServiceError(409, "InvalidTransition", f"cannot plan from {row['status']}")
        if not self.registry.transition_investigation(
            investigation_id, (row["status"],), "PLANNING"
        ):
            raise ServiceError(409, "InvalidTransition", "investigation changed during planning")
        try:
            catalogue = self.tools.call(investigation_id, "list_scenarios", {}).data["scenarios"]
            catalogue = [item for item in catalogue if item["kind"] == "scenario"]
            proposal = self.provider.generate_structured_plan(row["question"], catalogue)
            self._model_event(investigation_id, "PLANNING", "generate_structured_plan")
            self.registry.ai_event(
                investigation_id,
                "PLANNING",
                "classify_intent",
                "ok",
                {"intent": proposal.intent.value},
            )
            if proposal.clarification or proposal.intent == Intent.UNSUPPORTED:
                message = (
                    proposal.clarification
                    or "This request is outside the supported research design."
                )
                self.registry.transition_investigation(
                    investigation_id,
                    ("PLANNING",),
                    "UNSUPPORTED",
                    error_code="UnsupportedRequest",
                    error_message=message,
                )
                return self.view(investigation_id)
            if proposal.intent == Intent.ANALYZE_EXPERIMENT:
                self._analyze_existing(investigation_id, proposal)
                return self.view(investigation_id)
            if proposal.intent != Intent.COMPARE_STRATEGIES:
                raise ServiceError(
                    422,
                    "UnsupportedIntent",
                    "New plans currently support a paired maker comparison",
                )
            validation_started = time.perf_counter()
            unknown = self._unknown_metrics(proposal)
            if unknown:
                self.registry.ai_event(
                    investigation_id,
                    "PLANNING",
                    "validate_plan_metrics",
                    "rejected",
                    {"unsupported_metrics": unknown},
                )
                corrected = self.provider.correct_structured_plan(
                    row["question"], catalogue, proposal, unknown
                )
                self._model_event(investigation_id, "PLANNING", "correct_structured_plan")
                if (
                    corrected.intent != proposal.intent
                    or corrected.scenario_key != proposal.scenario_key
                    or corrected.seed_count != proposal.seed_count
                    or corrected.subject_agent_id != proposal.subject_agent_id
                ):
                    raise ServiceError(
                        422,
                        "InvalidCorrection",
                        "model correction changed the experiment design; submit a new request",
                    )
                if corrected.clarification:
                    raise ServiceError(
                        422,
                        "UnsupportedRequest",
                        corrected.clarification
                        or "No supported native metric answers this request",
                    )
                proposal = corrected
            plan = self._validated_plan(investigation_id, row["question"], proposal)
            self.registry.transition_investigation(
                investigation_id,
                ("PLANNING",),
                "AWAITING_APPROVAL",
                plan_json=plan.model_dump(mode="json"),
            )
            self.registry.ai_event(
                investigation_id,
                "AWAITING_APPROVAL",
                "validate_experiment",
                "ok",
                {
                    "config_digest": plan.config_digest,
                    "runs": plan.total_runs,
                    "duration_ms": round((time.perf_counter() - validation_started) * 1000),
                },
            )
        except ProviderError as error:
            self.registry.transition_investigation(
                investigation_id,
                ("PLANNING",),
                "FAILED",
                error_code=error.code,
                error_message=str(error),
            )
            self.registry.ai_event(
                investigation_id,
                "FAILED",
                "generate_structured_plan",
                "failed",
                {"code": error.code},
            )
        except ServiceError as error:
            self.registry.transition_investigation(
                investigation_id,
                ("PLANNING",),
                "UNSUPPORTED",
                error_code=error.code,
                error_message=error.message,
            )
            self.registry.ai_event(
                investigation_id,
                "UNSUPPORTED",
                "validate_experiment",
                "rejected",
                {"code": error.code, "details": error.details},
            )
        except Exception:
            self.registry.transition_investigation(
                investigation_id,
                ("PLANNING",),
                "FAILED",
                error_code="PlanningFailed",
                error_message="planning failed unexpectedly",
            )
            self.registry.ai_event(
                investigation_id, "FAILED", "planning", "failed", {"code": "PlanningFailed"}
            )
        return self.view(investigation_id)

    def _analyze_existing(self, investigation_id: str, proposal: Proposal) -> None:
        if not proposal.experiment_id:
            raise ServiceError(422, "AmbiguousExperiment", "Name a completed experiment ID")
        experiment = self.research.require_experiment(proposal.experiment_id)
        if experiment["status"] not in ("completed", "partially_completed"):
            raise ServiceError(409, "ExperimentNotCompleted", "the experiment has no final results")
        self.registry.transition_investigation(
            investigation_id, ("PLANNING",), "ANALYZING", experiment_id=proposal.experiment_id
        )
        try:
            report_started = time.perf_counter()
            report = self._make_report(investigation_id, proposal.experiment_id)
            status = "PARTIAL" if experiment["status"] == "partially_completed" else "REPORT_READY"
            self.registry.transition_investigation(
                investigation_id, ("ANALYZING",), status, report_json=report
            )
            self.registry.ai_event(
                investigation_id,
                status,
                "report_validated",
                "ok",
                {
                    "claim_count": len(report["claims"]),
                    "duration_ms": round((time.perf_counter() - report_started) * 1000),
                },
            )
        except (ProviderError, ServiceError) as error:
            self.registry.transition_investigation(
                investigation_id,
                ("ANALYZING",),
                "PARTIAL",
                error_code=getattr(error, "code", "ReportFailed"),
                error_message=str(error),
            )
            self.registry.ai_event(
                investigation_id,
                "PARTIAL",
                "report_generation",
                "failed",
                {"code": getattr(error, "code", "ReportFailed")},
            )

    def _validated_plan(
        self, investigation_id: str, question: str, proposal: Proposal
    ) -> ResearchPlan:
        if not proposal.scenario_key:
            raise ServiceError(422, "AmbiguousScenario", "Name one supported scenario")
        scenario = self.research.scenarios.get(proposal.scenario_key)
        if scenario is None or scenario.kind != "scenario":
            raise ServiceError(422, "UnknownScenario", "scenario is not in the supported catalogue")
        if not proposal.seed_count:
            raise ServiceError(422, "AmbiguousSeeds", "Specify a paired seed count")
        if proposal.seed_count > self.research.settings.ai_max_seeds:
            raise ServiceError(
                422,
                "SeedLimit",
                f"AI plans allow at most {self.research.settings.ai_max_seeds} paired seeds",
            )
        primary = proposal.primary_metric or "total_pnl"
        unknown = self._unknown_metrics(proposal)
        if unknown:
            raise ServiceError(
                422,
                "UnknownMetric",
                f"Unsupported comparison metric(s): {', '.join(unknown)}. "
                f"Choose native metric identifiers: {', '.join(NATIVE_EXPERIMENT_METRICS)}. "
                "For inventory exposure, use mean_abs_position or max_abs_position; "
                "'inventory risk' itself is an analysis topic.",
                {"unsupported_metrics": unknown, "supported_metrics": NATIVE_EXPERIMENT_METRICS},
            )
        agents = scenario.config.get("scenario", {}).get("agents", [])
        candidates = [
            agent for agent in agents if agent["policy"]["kind"] == "InventoryMarketMaker"
        ]
        if proposal.subject_agent_id:
            candidates = [agent for agent in candidates if agent["id"] == proposal.subject_agent_id]
        if len(candidates) != 1:
            raise ServiceError(
                422, "AmbiguousAgent", "select the inventory-aware maker in this scenario"
            )
        agent = candidates[0]
        aware = agent["policy"]
        baseline = {
            key: aware[key]
            for key in ("half_spread_ticks", "quote_size", "levels", "level_spacing_ticks")
        }
        baseline["kind"] = "MarketMaker"
        config: Json = {
            "schema_version": 1,
            "experiment_id": "ai_maker_comparison",
            "base_config": scenario.config,
            "horizon_ns": None,
            "seeds": {"first": 1, "count": proposal.seed_count},
            "variants": [
                {
                    "name": "baseline_maker",
                    "policy_overrides": [{"agent_id": agent["id"], "policy": baseline}],
                },
                {
                    "name": "inventory_maker",
                    "policy_overrides": [{"agent_id": agent["id"], "policy": aware}],
                },
            ],
            "comparison": {
                "baseline": "baseline_maker",
                "candidate": "inventory_maker",
                "subject_agent_id": agent["id"],
            },
            "bootstrap": {"resamples": 1000, "seed": 20261010},
            "artifacts": "None",
            "limits": {"max_runs": proposal.seed_count * 2, "max_output_bytes": 0},
        }
        validation = self.tools.call(
            investigation_id, "validate_experiment", {"config": config}
        ).data
        if not validation["valid"]:
            raise ServiceError(
                422,
                "InvalidExperiment",
                "native experiment validation rejected the plan",
                validation["errors"],
            )
        canonical = validation["canonical_config"]
        return ResearchPlan(
            question=question,
            intent=proposal.intent,
            hypothesis=proposal.hypothesis,
            scenario_key=scenario.key,
            scenario_digest=scenario.config_digest,
            subject_agent_id=agent["id"],
            seed_count=proposal.seed_count,
            primary_metric=primary,
            secondary_metrics=list(dict.fromkeys(proposal.secondary_metrics)),
            analysis_topics=proposal.analysis_topics,
            methodology="Native paired-seed maker comparison with t and bootstrap intervals",
            limitations=[
                *proposal.limitations,
                "Synthetic single-symbol market; no real-market calibration",
            ],
            total_runs=proposal.seed_count * 2,
            config_digest=_digest(canonical),
            experiment_config=canonical,
        )

    @staticmethod
    def _unknown_metrics(proposal: Proposal) -> list[str]:
        return list(
            dict.fromkeys(
                metric
                for metric in [proposal.primary_metric or "total_pnl", *proposal.secondary_metrics]
                if metric not in METRICS
            )
        )

    def approve(self, investigation_id: str, config_digest: str) -> Json:
        row = self.require(investigation_id)
        plan = row["plan"]
        if not plan or plan["config_digest"] != config_digest:
            raise ServiceError(
                409, "PlanDigestMismatch", "approval must name the current validated plan digest"
            )
        if not self.registry.transition_investigation(
            investigation_id, ("AWAITING_APPROVAL",), "APPROVED"
        ):
            current = self.require(investigation_id)
            if current["status"] in (
                "APPROVED",
                "EXECUTING",
                "ANALYZING",
                "REPORT_READY",
                "PARTIAL",
            ):
                return self.view(investigation_id)
            raise ServiceError(409, "InvalidTransition", f"cannot approve from {current['status']}")
        self.registry.ai_event(
            investigation_id, "APPROVED", "human_approved", "ok", {"config_digest": config_digest}
        )
        try:
            experiment = self.research.create_experiment(
                plan["experiment_config"], plan["scenario_key"]
            )
        except ServiceError as error:
            self.registry.transition_investigation(
                investigation_id,
                ("APPROVED",),
                "FAILED",
                error_code=error.code,
                error_message=error.message,
            )
            return self.view(investigation_id)
        self.registry.transition_investigation(
            investigation_id, ("APPROVED",), "EXECUTING", experiment_id=experiment["id"]
        )
        self.registry.ai_event(
            investigation_id,
            "EXECUTING",
            "experiment_submitted",
            "ok",
            {"experiment_id": experiment["id"]},
        )
        self._monitor(investigation_id, experiment["id"])
        return self.view(investigation_id)

    def _monitor(self, investigation_id: str, experiment_id: str) -> None:
        thread = threading.Thread(
            target=self._wait_and_report,
            args=(investigation_id, experiment_id),
            daemon=True,
            name="markettwin-ai-monitor",
        )
        self._threads.append(thread)
        thread.start()

    def _wait_and_report(self, investigation_id: str, experiment_id: str) -> None:
        while not self._closed.wait(0.2):
            experiment = self.registry.experiment(experiment_id)
            if experiment is None or experiment["status"] in ("pending", "running"):
                continue
            status = experiment["status"]
            if status not in ("completed", "partially_completed"):
                terminal = "CANCELLED" if status == "canceled" else "FAILED"
                self.registry.transition_investigation(
                    investigation_id,
                    ("EXECUTING",),
                    terminal,
                    error_code=experiment["error_code"] or "ExperimentFailed",
                    error_message=experiment["error_message"] or status,
                )
                self.registry.ai_event(investigation_id, terminal, "experiment_finished", status)
                return
            if not self.registry.transition_investigation(
                investigation_id, ("EXECUTING",), "ANALYZING"
            ):
                return
            self.registry.ai_event(
                investigation_id,
                "ANALYZING",
                "results_retrieved",
                "ok",
                {"experiment_id": experiment_id},
            )
            try:
                report_started = time.perf_counter()
                report = self._make_report(investigation_id, experiment_id)
                terminal = "PARTIAL" if status == "partially_completed" else "REPORT_READY"
                self.registry.transition_investigation(
                    investigation_id, ("ANALYZING",), terminal, report_json=report
                )
                self.registry.ai_event(
                    investigation_id,
                    terminal,
                    "report_validated",
                    "ok",
                    {
                        "claim_count": len(report["claims"]),
                        "duration_ms": round((time.perf_counter() - report_started) * 1000),
                    },
                )
            except (ProviderError, ServiceError) as error:
                self.registry.transition_investigation(
                    investigation_id,
                    ("ANALYZING",),
                    "PARTIAL",
                    error_code=getattr(error, "code", "ReportFailed"),
                    error_message=str(error),
                )
                self.registry.ai_event(
                    investigation_id,
                    "PARTIAL",
                    "report_generation",
                    "failed",
                    {"code": getattr(error, "code", "ReportFailed")},
                )
            except Exception:
                self.registry.transition_investigation(
                    investigation_id,
                    ("ANALYZING",),
                    "PARTIAL",
                    error_code="ReportFailed",
                    error_message="report generation failed unexpectedly",
                )
                self.registry.ai_event(
                    investigation_id,
                    "PARTIAL",
                    "report_generation",
                    "failed",
                    {"code": "ReportFailed"},
                )
            return

    def _make_report(self, investigation_id: str, experiment_id: str) -> Json:
        row = self.require(investigation_id)
        self.tools.call(
            investigation_id, "get_experiment_results", {"experiment_id": experiment_id}
        )
        document = self.research.experiment_document(experiment_id)
        results = document["results"]
        comparison = results.get("comparison")
        if not comparison or not comparison.get("metrics"):
            raise ServiceError(422, "MissingEvidence", "native results have no paired comparison")
        claims: list[Json] = []
        for metric in comparison["metrics"]:
            delta = metric["difference"]
            claim = Claim(
                metric=metric["name"],
                scope=metric["scope"],
                unit=metric["unit"],
                pairs=metric["pairs"],
                baseline_mean_milli=metric["baseline"]["mean_milli"]
                if metric["baseline"]
                else None,
                candidate_mean_milli=metric["candidate"]["mean_milli"]
                if metric["candidate"]
                else None,
                difference_mean_milli=delta["mean_milli"] if delta else None,
                difference_t_interval_milli=delta["mean_t_interval_milli"] if delta else None,
                difference_bootstrap_interval_milli=delta["mean_bootstrap_interval_milli"]
                if delta
                else None,
                evidence_ref=f"experiment:{experiment_id}:metric:{metric['name']}",
            )
            claims.append(claim.model_dump(mode="json"))
        interpretation = self.provider.synthesize_research_report(
            {
                "question": row["question"],
                "hypothesis": row["plan"]["hypothesis"] if row["plan"] else row["question"],
                "experiment_id": experiment_id,
                "claims": claims[:12],
            }
        )
        self._model_event(investigation_id, "ANALYZING", "synthesize_research_report")
        prose = [
            interpretation.interpretation,
            *interpretation.limitations,
            *interpretation.follow_up,
        ]
        if not all(_no_numbers(text) for text in prose):
            raise ServiceError(
                422, "UnverifiedNumericalClaim", "model prose included an unverified number"
            )
        report: Json = {
            "version": 1,
            "question": row["question"],
            "hypothesis": row["plan"]["hypothesis"] if row["plan"] else row["question"],
            "plan": row["plan"],
            "experiment_id": experiment_id,
            "engine_version": document["engine_version"],
            "config_digest": results["config_digest"],
            "results_digest": document["results_digest"],
            "baseline": comparison["baseline"],
            "candidate": comparison["candidate"],
            "pairs_completed": comparison["pairs_completed"],
            "claims": claims,
            "interpretation": interpretation.interpretation,
            "limitations": interpretation.limitations,
            "follow_up": interpretation.follow_up,
            "replay_status": "not_checked; materialize a cell and request replay verification",
        }
        verify_claims(report, document)
        return report

    def reject(self, investigation_id: str) -> Json:
        if not self.registry.transition_investigation(
            investigation_id, ("AWAITING_APPROVAL",), "REJECTED"
        ):
            raise ServiceError(409, "InvalidTransition", "only an awaiting plan may be rejected")
        self.registry.ai_event(investigation_id, "REJECTED", "human_rejected", "ok")
        return self.view(investigation_id)

    def cancel(self, investigation_id: str) -> Json:
        row = self.require(investigation_id)
        if row["status"] == "AWAITING_APPROVAL":
            self.registry.transition_investigation(
                investigation_id, ("AWAITING_APPROVAL",), "CANCELLED"
            )
        elif row["status"] == "EXECUTING" and row["experiment_id"]:
            self.research.cancel_experiment(row["experiment_id"])
        else:
            raise ServiceError(409, "InvalidTransition", f"cannot cancel from {row['status']}")
        self.registry.ai_event(investigation_id, "CANCELLED", "cancel_requested", "ok")
        return self.view(investigation_id)

    def message(self, investigation_id: str, content: str) -> Json:
        row = self.require(investigation_id)
        if row["status"] not in ("REPORT_READY", "PARTIAL") or not row["report"]:
            raise ServiceError(409, "ReportNotReady", "follow-up analysis needs a completed report")
        if len(self.registry.ai_messages(investigation_id)) >= 30:
            raise ServiceError(429, "MessageLimit", "conversation limit reached")
        self.registry.ai_message(investigation_id, "user", content)
        report = row["report"]
        seed_evidence = None
        match = re.search(r"\bseed\s+(\d+)\b", content, re.IGNORECASE)
        if match:
            seed_evidence = self.seed(investigation_id, int(match.group(1)))
        elif re.search(r"\bworst.{0,30}seed\b", content, re.IGNORECASE):
            values = self.research.paired_values(report["experiment_id"])
            pnl = next((m for m in values["metrics"] if m["name"] == "total_pnl"), None)
            if pnl is not None:
                candidates = [
                    (value, seed)
                    for value, seed in zip(pnl["candidate"], values["seeds"], strict=True)
                    if value is not None
                ]
                if candidates:
                    seed_evidence = self.seed(investigation_id, min(candidates)[1])
        context = {
            "question": report["question"],
            "claims": report["claims"],
            "experiment_id": report["experiment_id"],
        }
        if seed_evidence:
            context["selected_seed"] = seed_evidence
        try:
            answer = self.provider.answer_followup(content, context)
        except ProviderError as error:
            self.registry.ai_event(
                investigation_id,
                row["status"],
                "answer_followup_model",
                "failed",
                {"code": error.code},
            )
            raise ServiceError(503, error.code, str(error)) from error
        self._model_event(investigation_id, row["status"], "answer_followup_model")
        known = {claim["evidence_ref"] for claim in report["claims"]}
        if seed_evidence:
            known.add(seed_evidence["evidence_ref"])
        if not _no_numbers(answer.answer) or any(ref not in known for ref in answer.evidence_refs):
            raise ServiceError(
                422, "UnverifiedAnswer", "follow-up answer contains unsupported evidence"
            )
        answer_text = answer.answer
        if seed_evidence:
            pnl = seed_evidence["metrics"].get("total_pnl")
            if pnl:
                answer_text += (
                    f"\nNative seed {seed_evidence['seed']} total P&L: "
                    f"baseline {pnl['baseline']} {pnl['unit']}; "
                    f"candidate {pnl['candidate']} {pnl['unit']}; "
                    f"difference {pnl['difference']} {pnl['unit']}."
                )
            answer.evidence_refs.append(seed_evidence["evidence_ref"])
        self.registry.ai_message(investigation_id, "assistant", answer_text, answer.evidence_refs)
        self.registry.ai_event(
            investigation_id,
            row["status"],
            "answer_followup",
            "ok",
            {"evidence_refs": answer.evidence_refs},
        )
        return self.view(investigation_id)

    def _model_event(self, investigation_id: str, stage: str, operation: str) -> None:
        usage = getattr(self.provider, "last_usage", {})
        latency = getattr(self.provider, "last_latency_ms", None)
        cost = getattr(self.provider, "last_cost_usd", None)
        self.registry.ai_event(
            investigation_id,
            stage,
            operation,
            "ok",
            {
                "provider": self.research.settings.ai_provider,
                "model": self.research.settings.ai_model,
                "latency_ms": latency,
                "prompt_tokens": usage.get("prompt_tokens"),
                "completion_tokens": usage.get("completion_tokens"),
                "estimated_cost_usd": cost,
            },
        )

    def seed(self, investigation_id: str, seed: int) -> Json:
        row = self.require(investigation_id)
        if not row["experiment_id"] or row["status"] not in ("REPORT_READY", "PARTIAL"):
            raise ServiceError(409, "ResultsNotReady", "paired seed evidence is not ready")
        values = self.research.paired_values(row["experiment_id"])
        try:
            index = values["seeds"].index(seed)
        except ValueError as error:
            raise ServiceError(404, "SeedNotFound", "no completed pair has that seed") from error
        return {
            "seed": seed,
            "experiment_id": row["experiment_id"],
            "evidence_ref": f"experiment:{row['experiment_id']}:seed:{seed}",
            "metrics": {
                metric["name"]: {
                    "unit": metric["unit"],
                    "baseline": metric["baseline"][index],
                    "candidate": metric["candidate"][index],
                    "difference": metric["difference"][index],
                }
                for metric in values["metrics"]
            },
        }

    def markdown(self, investigation_id: str) -> str:
        row = self.require(investigation_id)
        report = row["report"]
        if report is None:
            raise ServiceError(409, "ReportNotReady", "this investigation has no report yet")
        verify_claims(report, self.research.experiment_document(report["experiment_id"]))

        def escape(value: str) -> str:
            return (
                value.replace("&", "&amp;")
                .replace("<", "&lt;")
                .replace(">", "&gt;")
                .replace("|", "\\|")
                .replace("\n", " ")
            )

        lines = [
            "# MarketTwin research report",
            "",
            "## Research Question",
            "",
            escape(report["question"]),
            "",
            "## Experiment Design",
            "",
            f"- Experiment: `{report['experiment_id']}`",
            f"- Baseline: {report['baseline']}",
            f"- Candidate: {report['candidate']}",
            f"- Completed pairs: {report['pairs_completed']}",
            f"- Engine: {report['engine_version']}",
            "",
            "## Quantitative Evidence",
            "",
            "| Metric | Baseline mean | Candidate mean | Paired difference | Evidence |",
            "| --- | ---: | ---: | ---: | --- |",
        ]

        def value(number: int | None, unit: str) -> str:
            return "undefined" if number is None else f"{Decimal(number) / Decimal(1000)} {unit}"

        for claim in report["claims"]:
            lines.append(
                "| "
                + " | ".join(
                    [
                        escape(claim["metric"]),
                        value(claim["baseline_mean_milli"], claim["unit"]),
                        value(claim["candidate_mean_milli"], claim["unit"]),
                        value(claim["difference_mean_milli"], claim["unit"]),
                        f"`{claim['evidence_ref']}`",
                    ]
                )
                + " |"
            )
        lines.extend(
            [
                "",
                "## Interpretation",
                "",
                escape(report["interpretation"]),
                "",
                "## Limitations",
                "",
            ]
        )
        lines.extend(f"- {escape(item)}" for item in report["limitations"])
        lines.extend(
            [
                "",
                "## Reproducibility",
                "",
                f"- Config digest: `{report['config_digest']}`",
                f"- Results digest: `{report['results_digest']}`",
                f"- Replay: {escape(report['replay_status'])}",
                "",
                "## Follow-Up Research",
                "",
            ]
        )
        lines.extend(f"- {escape(item)}" for item in report["follow_up"])
        return "\n".join(lines) + "\n"


def verify_claims(report: Json, document: Json) -> None:
    """Reject modified values, units, signs, labels, counts and unresolved references."""
    comparison = document["results"]["comparison"]
    experiment_id = report["experiment_id"]
    for key, native in (
        ("engine_version", document["engine_version"]),
        ("config_digest", document["results"]["config_digest"]),
        ("results_digest", document["results_digest"]),
        ("baseline", comparison["baseline"]),
        ("candidate", comparison["candidate"]),
    ):
        if report.get(key) != native:
            raise ServiceError(422, "ClaimMismatch", f"{key} differs from native results")
    authoritative = {m["name"]: m for m in comparison["metrics"]}
    if report["pairs_completed"] != comparison["pairs_completed"]:
        raise ServiceError(422, "ClaimMismatch", "paired seed count differs from native results")
    for raw in report["claims"]:
        claim = Claim.model_validate(raw)
        metric = authoritative.get(claim.metric)
        if (
            metric is None
            or claim.evidence_ref != f"experiment:{experiment_id}:metric:{claim.metric}"
        ):
            raise ServiceError(422, "ClaimMismatch", "claim has no authoritative metric evidence")
        delta = metric["difference"]
        expected = Claim(
            metric=metric["name"],
            scope=metric["scope"],
            unit=metric["unit"],
            pairs=metric["pairs"],
            baseline_mean_milli=metric["baseline"]["mean_milli"] if metric["baseline"] else None,
            candidate_mean_milli=metric["candidate"]["mean_milli"] if metric["candidate"] else None,
            difference_mean_milli=delta["mean_milli"] if delta else None,
            difference_t_interval_milli=delta["mean_t_interval_milli"] if delta else None,
            difference_bootstrap_interval_milli=delta["mean_bootstrap_interval_milli"]
            if delta
            else None,
            evidence_ref=claim.evidence_ref,
        )
        if claim != expected:
            raise ServiceError(422, "ClaimMismatch", f"{claim.metric} differs from native results")
