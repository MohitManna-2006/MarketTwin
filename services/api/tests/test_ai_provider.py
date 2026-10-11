"""Provider transport contract without paid or network model calls."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import httpx
import pytest

from markettwin_api.ai_models import NATIVE_EXPERIMENT_METRICS, Proposal
from markettwin_api.ai_provider import OpenAICompatibleProvider, ProviderError
from markettwin_api.settings import Settings


def configured(tmp_path: Path) -> Settings:
    return Settings(
        workspace=tmp_path,
        ai_model="test-model",
        ai_api_key="secret-test-key",
        ai_base_url="https://model.example/v1",
        ai_input_usd_per_million_tokens="1",
        ai_output_usd_per_million_tokens="2",
    )


def test_json_request_and_usage_are_backend_only(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    captured: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        captured.append(request)
        return httpx.Response(
            200,
            json={
                "choices": [
                    {
                        "message": {
                            "content": json.dumps(
                                {"intent": "unsupported", "hypothesis": "Out of scope"}
                            )
                        }
                    }
                ],
                "usage": {"prompt_tokens": 17, "completion_tokens": 9},
            },
        )

    original = httpx.Client
    transport = httpx.MockTransport(handler)
    monkeypatch.setattr(httpx, "Client", lambda **kwargs: original(transport=transport, **kwargs))
    provider = OpenAICompatibleProvider(configured(tmp_path))
    proposal = provider.generate_structured_plan("Can you connect to a broker?", [])
    assert proposal.intent == "unsupported"
    assert provider.last_usage == {"prompt_tokens": 17, "completion_tokens": 9}
    assert provider.last_cost_usd == "0.000035"
    assert len(captured) == 1
    assert str(captured[0].url) == "https://model.example/v1/chat/completions"
    assert captured[0].headers["authorization"] == "Bearer secret-test-key"
    body: dict[str, Any] = json.loads(captured[0].content)
    assert body["model"] == "test-model"
    assert body["response_format"] == {"type": "json_object"}
    assert "secret-test-key" not in captured[0].content.decode()


def test_authentication_error_has_no_secret(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    original = httpx.Client
    transport = httpx.MockTransport(lambda request: httpx.Response(401, json={"secret": "bad"}))
    monkeypatch.setattr(httpx, "Client", lambda **kwargs: original(transport=transport, **kwargs))
    provider = OpenAICompatibleProvider(configured(tmp_path))
    with pytest.raises(ProviderError) as captured:
        provider.generate_structured_plan("question", [])
    assert captured.value.code == "ProviderAuthentication"
    assert "secret-test-key" not in str(captured.value)


def test_plan_schema_advertises_exact_native_metric_identifiers() -> None:
    properties = Proposal.model_json_schema()["properties"]
    assert properties["primary_metric"]["enum"] == list(NATIVE_EXPERIMENT_METRICS)
    assert properties["secondary_metrics"]["items"]["enum"] == list(NATIVE_EXPERIMENT_METRICS)
    assert "inventory_risk" not in NATIVE_EXPERIMENT_METRICS


def test_provider_correction_receives_rejected_metric_and_original_question(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    requests: list[dict[str, Any]] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(json.loads(request.content))
        return httpx.Response(
            200,
            json={
                "choices": [
                    {
                        "message": {
                            "content": json.dumps(
                                {
                                    "intent": "compare_strategies",
                                    "scenario_key": "baseline",
                                    "seed_count": 10,
                                    "primary_metric": "total_pnl",
                                    "secondary_metrics": ["mean_abs_position", "max_abs_position"],
                                    "analysis_topics": ["inventory risk"],
                                    "hypothesis": "Compare makers.",
                                }
                            )
                        }
                    }
                ],
            },
        )

    original = httpx.Client
    transport = httpx.MockTransport(handler)
    monkeypatch.setattr(httpx, "Client", lambda **kwargs: original(transport=transport, **kwargs))
    provider = OpenAICompatibleProvider(configured(tmp_path))
    bad = Proposal(
        intent="compare_strategies",
        scenario_key="baseline",
        seed_count=10,
        primary_metric="total_pnl",
        secondary_metrics=["inventory_risk"],
        hypothesis="Compare makers.",
    )
    corrected = provider.correct_structured_plan(
        "Compare makers across 10 paired seeds and explain inventory risk",
        [],
        bad,
        ["inventory_risk"],
    )
    assert corrected.secondary_metrics == ["mean_abs_position", "max_abs_position"]
    assert len(requests) == 1
    payload = json.loads(requests[0]["messages"][1]["content"])
    assert payload["unsupported_metrics"] == ["inventory_risk"]
    assert payload["question"].startswith("Compare makers")
