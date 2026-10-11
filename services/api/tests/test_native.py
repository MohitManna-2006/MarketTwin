"""The binding boundary (architecture appendix A.18), against the real core."""

from __future__ import annotations

import json
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import markettwin
import pytest

from tests.conftest import CONFIGS, REPOSITORY, SECOND, load_config, short

GOLDEN = REPOSITORY / "core" / "tests" / "replay" / "golden"


def smoke_experiment() -> dict:
    experiment = json.loads((CONFIGS / "experiments" / "maker_comparison_smoke.json").read_text())
    experiment["base_config"] = load_config("baseline")
    experiment["artifacts"] = "None"
    experiment["limits"]["max_output_bytes"] = 0
    return experiment


def test_reports_the_linked_engine_and_its_schemas() -> None:
    info = markettwin.build_info()
    assert markettwin.version() == info["engine_version"] == "0.4.0"
    assert {info["experiment_schema"], info["accounting_schema"], info["metrics_schema"]} == {1}
    assert info["compiler"] and info["build_type"]


def test_a_run_through_the_binding_is_the_run_the_cli_golden_pins(tmp_path: Path) -> None:
    golden = json.loads((GOLDEN / "phase2a_baseline_summary.json").read_text())
    config = (CONFIGS / "baseline.json").read_text()
    canonical, digest = markettwin.canonicalize_config(config)
    assert digest == golden["config_digest"]
    assert json.loads(canonical)["seed"] == 42

    in_memory = markettwin.run(config)
    written = markettwin.run(config, tmp_path / "baseline")
    assert in_memory == written == golden
    assert sorted(path.name for path in (tmp_path / "baseline").iterdir()) == [
        "accounting.jsonl",
        "commands.jsonl",
        "config.json",
        "manifest.json",
        "metrics.json",
        "trace.jsonl",
    ]
    metrics = json.loads((tmp_path / "baseline" / "metrics.json").read_text())
    assert metrics == json.loads((GOLDEN / "phase2b_baseline_metrics.json").read_text())

    verdict = markettwin.verify_replay(tmp_path / "baseline")
    assert verdict["verified"] is True
    assert verdict["code"] == "Verified"
    assert verdict["summary"] == golden


def test_a_dict_and_its_text_are_the_same_config() -> None:
    config = load_config("fundamental_shock")
    assert markettwin.canonicalize_config(config) == markettwin.canonicalize_config(
        json.dumps(config)
    )


def test_native_errors_keep_their_code_and_sequence(tmp_path: Path) -> None:
    with pytest.raises(markettwin.NativeRunError) as invalid:
        markettwin.canonicalize_config({"schema_version": 2})
    assert invalid.value.code == "InvalidConfig"
    assert invalid.value.sequence == 0

    with pytest.raises(markettwin.NativeRunError) as malformed:
        markettwin.canonicalize_config("{not json")
    assert malformed.value.code == "InvalidJson"

    with pytest.raises(markettwin.NativeRunError) as unsupported:
        markettwin.canonicalize_config({"schema_version": 9})
    assert unsupported.value.code == "UnsupportedSchema"

    config = load_config("baseline")
    config["scenario"]["agents"][0]["wakeup"]["distribution"] = "Sometimes"
    with pytest.raises(markettwin.NativeRunError, match="unknown enum spelling: Sometimes"):
        markettwin.canonicalize_config(config)

    # A run that exhausts its byte budget fails at the record that did not fit.
    starved = short(load_config("baseline"), 60) | {"max_trace_bytes": 4096}
    with pytest.raises(markettwin.NativeRunError) as budget:
        markettwin.run(starved)
    assert budget.value.code == "ResourceLimit"
    assert budget.value.sequence > 0

    markettwin.run(short(load_config("baseline"), 5), tmp_path / "once")
    with pytest.raises(markettwin.NativeRunError) as exists:
        markettwin.run(short(load_config("baseline"), 5), tmp_path / "once")
    assert exists.value.code == "OutputExists"
    assert isinstance(exists.value, RuntimeError)

    missing = markettwin.verify_replay(tmp_path / "nothing-here")
    assert missing["verified"] is False
    assert missing["code"] == "IncompleteArtifacts"


def test_independent_runs_on_threads_match_the_same_runs_in_sequence() -> None:
    base = short(load_config("baseline"), 300)
    configs = [base | {"seed": seed} for seed in (1, 2, 3, 4, 5, 6)]
    sequential = [markettwin.run(config) for config in configs]
    with ThreadPoolExecutor(max_workers=6) as pool:
        concurrent = list(pool.map(markettwin.run, configs))
    assert concurrent == sequential
    assert len({summary["trace_digest"] for summary in sequential}) == len(configs)


def test_the_gil_is_released_while_a_run_is_in_progress() -> None:
    config = load_config("baseline") | {"horizon_ns": 4 * 3600 * SECOND}
    done = threading.Event()
    worker = threading.Thread(target=lambda: (markettwin.run(config), done.set()))
    worker.start()
    ticks = 0
    while not done.is_set():
        time.sleep(0.005)
        ticks += 1
    worker.join()
    # A call that held the GIL would let this thread run only after it returned.
    assert ticks >= 20


def test_an_experiment_reports_progress_and_the_values_behind_its_statistics() -> None:
    experiment = smoke_experiment()
    progress = markettwin.ExperimentProgress()
    output = markettwin.run_experiment(experiment, progress=progress)
    assert (progress.total, progress.completed, progress.failed) == (8, 8, 0)
    assert progress.current_run_id == "maker_comparison_smoke-inventory_maker-s4"
    assert output.results["design"] == {
        "pairing": "same_seed",
        "seeds": 4,
        "total_runs": 8,
        "variants": 2,
    }
    values = output.paired_values
    assert values["seeds"] == [1, 2, 3, 4]
    assert len(values["metrics"]) == len(output.results["comparison"]["metrics"]) == 19
    for metric, column in zip(
        output.results["comparison"]["metrics"], values["metrics"], strict=True
    ):
        assert (metric["name"], metric["unit"]) == (column["name"], column["unit"])
        differences = [value for value in column["difference"] if value is not None]
        assert len(differences) == metric["pairs"]
        if differences:
            assert metric["difference"]["minimum"] == min(differences)
            assert metric["difference"]["maximum"] == max(differences)
            exact_mean_milli = sum(differences) * 1000 / len(differences)
            assert abs(metric["difference"]["mean_milli"] - exact_mean_milli) <= 0.5

    # The same experiment again is the same document, byte for byte.
    assert markettwin.run_experiment(experiment).results == output.results


def test_a_cell_run_alone_has_the_digests_the_experiment_recorded() -> None:
    experiment = smoke_experiment()
    results = markettwin.run_experiment(experiment).results
    recorded = next(
        run for run in results["runs"] if run["variant"] == "baseline_maker" and run["seed"] == 3
    )
    cell = markettwin.experiment_cell_config(experiment, "baseline_maker", 3)
    assert json.loads(cell)["run_id"] == recorded["run_id"]
    summary = markettwin.run(cell)
    for key in ("config_digest", "commands_digest", "trace_digest", "book_digest", "trade_digest"):
        assert summary[key] == recorded[key]
    with pytest.raises(markettwin.NativeRunError, match="no variant named nobody"):
        markettwin.experiment_cell_config(experiment, "nobody", 3)


def test_a_canceled_experiment_records_every_remaining_cell_as_canceled() -> None:
    experiment = smoke_experiment()
    progress = markettwin.ExperimentProgress()
    progress.request_cancel()
    output = markettwin.run_experiment(experiment, progress=progress)
    assert {run["error_code"] for run in output.results["runs"]} == {"Canceled"}
    assert (progress.completed, progress.failed) == (0, 8)
    assert output.results["comparison"]["pairs_completed"] == 0
    assert output.paired_values["seeds"] == []
