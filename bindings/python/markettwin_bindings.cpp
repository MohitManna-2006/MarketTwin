// markettwin._native
//
// The only boundary between Python and the simulation. Every function is one complete
// native operation: a config goes in as JSON text and a summary, a replay verdict, or a
// results document comes out as JSON text. Nothing here is called per event, trade, or
// price level, and no Python object is created while a run is in progress: the GIL is
// released for the whole native call.
//
// LiveNormalizer is the one object that outlives a call: it holds the last observed state
// of each live instrument. It is still driven by coarse calls, one per batch of provider
// frames, never one per record.

#include "markettwin/calibration/calibration.hpp"
#include "markettwin/common/hash.hpp"
#include "markettwin/common/run_error.hpp"
#include "markettwin/common/version.hpp"
#include "markettwin/experiment/experiment.hpp"
#include "markettwin/historical/historical.hpp"
#include "markettwin/live/live.hpp"
#include "markettwin/simulation/simulation.hpp"
#include "markettwin/telemetry/replay.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <string>
#include <utility>

namespace py = pybind11;

namespace {

// Counters a running experiment updates from native code and Python reads from another
// thread. The experiment never calls into Python.
class ExperimentProgress {
public:
  void begin(std::uint64_t total) noexcept {
    total_.store(total);
    completed_.store(0);
    failed_.store(0);
  }
  void start(const std::string& run_id) {
    const std::scoped_lock lock{mutex_};
    current_ = run_id;
  }
  void finish(bool completed) noexcept { ++(completed ? completed_ : failed_); }
  void request_cancel() noexcept { cancel_.store(true); }

  [[nodiscard]] std::uint64_t total() const noexcept { return total_.load(); }
  [[nodiscard]] std::uint64_t completed() const noexcept { return completed_.load(); }
  [[nodiscard]] std::uint64_t failed() const noexcept { return failed_.load(); }
  [[nodiscard]] bool cancel_requested() const noexcept { return cancel_.load(); }
  [[nodiscard]] std::string current_run_id() const {
    const std::scoped_lock lock{mutex_};
    return current_;
  }

private:
  std::atomic<std::uint64_t> total_{0};
  std::atomic<std::uint64_t> completed_{0};
  std::atomic<std::uint64_t> failed_{0};
  std::atomic<bool> cancel_{false};
  mutable std::mutex mutex_;
  std::string current_;
};

// Raised for every markettwin::RunError, with its stable code and sequence attached.
py::object& native_error_type() {
  static auto* type = new py::object{};
  return *type;
}

void translate(std::exception_ptr pointer) {
  try {
    if (pointer) {
      std::rethrow_exception(pointer);
    }
  } catch (const markettwin::RunError& error) {
    py::object instance = native_error_type()(error.what());
    instance.attr("code") = error.code();
    instance.attr("sequence") = error.sequence().value();
    PyErr_SetObject(native_error_type().ptr(), instance.ptr());
  }
}

py::tuple canonicalize_config(const std::string& text) {
  std::string canonical;
  {
    const py::gil_scoped_release release;
    canonical = markettwin::canonical_config(markettwin::parse_config(text));
  }
  return py::make_tuple(canonical, markettwin::sha256(canonical));
}

std::string run(const std::string& config_text, const std::string& output_directory) {
  const py::gil_scoped_release release;
  markettwin::Simulation simulation{markettwin::parse_config(config_text),
                                    std::filesystem::path{output_directory}};
  return markettwin::summary_json(simulation.run());
}

std::string verify_replay(const std::string& run_directory) {
  const py::gil_scoped_release release;
  return markettwin::replay_result_json(
      markettwin::verify_replay(std::filesystem::path{run_directory}));
}

std::string canonicalize_experiment(const std::string& text, const std::string& base_directory) {
  const py::gil_scoped_release release;
  return markettwin::canonical_experiment(
      markettwin::parse_experiment(text, std::filesystem::path{base_directory}));
}

std::string experiment_cell_config(const std::string& text, const std::string& base_directory,
                                   const std::string& variant, std::uint64_t seed) {
  const py::gil_scoped_release release;
  const auto config = markettwin::parse_experiment(text, std::filesystem::path{base_directory});
  for (const auto& item : config.variants) {
    if (item.name == variant) {
      return markettwin::canonical_config(markettwin::variant_config(config, item, seed));
    }
  }
  throw markettwin::RunError{"InvalidConfig", "the experiment has no variant named " + variant};
}

py::tuple run_experiment(const std::string& text, const std::string& base_directory,
                         const std::string& output_directory,
                         const std::shared_ptr<ExperimentProgress>& progress) {
  std::string results;
  std::string values;
  {
    const py::gil_scoped_release release;
    const auto config = markettwin::parse_experiment(text, std::filesystem::path{base_directory});
    if (progress) {
      progress->begin(config.seeds.size() * config.variants.size());
    }
    // Same work as the runner's default, plus the counters. A canceled experiment still
    // records every remaining cell, as failed with code Canceled, and still publishes.
    const markettwin::RunFunction execute = [progress](const markettwin::RunConfig& cell,
                                                       const std::filesystem::path& directory) {
      if (!progress) {
        return markettwin::Simulation{cell, directory}.run();
      }
      if (progress->cancel_requested()) {
        progress->finish(false);
        throw markettwin::RunError{"Canceled", "the experiment was canceled before this run"};
      }
      progress->start(cell.run_id);
      try {
        auto summary = markettwin::Simulation{cell, directory}.run();
        progress->finish(true);
        return summary;
      } catch (...) {
        progress->finish(false);
        throw;
      }
    };
    const auto result =
        markettwin::run_experiment(config, std::filesystem::path{output_directory}, execute);
    results = markettwin::experiment_results_json(config, result);
    values = markettwin::experiment_paired_values_json(config, result);
  }
  return py::make_tuple(results, values);
}

py::dict build_info() {
  py::dict info;
  info["engine_version"] = std::string{markettwin::version()};
  info["git_revision"] = MARKETTWIN_GIT_REVISION;
  info["compiler"] = MARKETTWIN_COMPILER;
  info["build_type"] = MARKETTWIN_BUILD_TYPE;
  info["experiment_schema"] = markettwin::kExperimentSchema;
  info["accounting_schema"] = markettwin::kAccountingSchema;
  info["metrics_schema"] = markettwin::kMetricsSchema;
  info["historical_schema"] = markettwin::historical::kHistoricalSchema;
  info["historical_normalization_version"] =
      markettwin::historical::kHistoricalNormalizationVersion;
  info["live_schema"] = markettwin::live::kLiveSchema;
  info["calibration_schema"] = markettwin::calibration::kCalibrationSchema;
  info["max_experiment_runs"] = markettwin::kMaxExperimentRuns;
  return info;
}

} // namespace

PYBIND11_MODULE(_native, module) {
  module.doc() = "Run-level interface to the MarketTwin C++ core.";

  native_error_type() =
      py::exception<markettwin::RunError>(module, "NativeRunError", PyExc_RuntimeError);
  py::register_exception_translator(&translate);

  module.def(
      "version", [] { return std::string{markettwin::version()}; },
      "Engine version of the linked core.");
  module.def("build_info", &build_info, "Engine version, build provenance, and schema versions.");
  module.def("canonicalize_config", &canonicalize_config, py::arg("config_text"),
             "Parse and validate a run config. Returns (canonical JSON, config digest).");
  module.def("run", &run, py::arg("config_text"), py::arg("output_directory") = std::string{},
             "Run one simulation. With a directory (which must not exist) it writes the run's "
             "artifacts there. Returns the terminal summary as JSON.");
  module.def("verify_replay", &verify_replay, py::arg("run_directory"),
             "Regenerate a recorded run and compare every record. Returns the verdict as JSON.");
  module.def(
      "import_historical_jsonl",
      [](const std::string& input, const std::string& output, const std::string& metadata) {
        const py::gil_scoped_release release;
        return markettwin::historical::import_jsonl(input, output, metadata);
      },
      py::arg("input"), py::arg("output"), py::arg("metadata_json"),
      "Normalize one bounded historical input into a separate observed-market session.");
  module.def(
      "verify_historical",
      [](const std::string& directory) {
        const py::gil_scoped_release release;
        return markettwin::historical::verify(directory);
      },
      py::arg("session_directory"),
      "Verify historical artifact hashes and deterministic L1 playback.");
  py::class_<markettwin::live::LiveNormalizer>(module, "LiveNormalizer")
      .def(py::init<const std::string&>(), py::arg("config_json"))
      .def(
          "process_batch",
          [](markettwin::live::LiveNormalizer& normalizer, const std::string& batch) {
            const py::gil_scoped_release release;
            return normalizer.process_batch(batch);
          },
          py::arg("batch_json"),
          "Normalize one batch of provider frames. Returns events, rejects, and the changed "
          "instrument states as JSON.")
      .def(
          "snapshot",
          [](const markettwin::live::LiveNormalizer& normalizer, std::int64_t now_wall_ns,
             std::int64_t now_monotonic_ns) {
            const py::gil_scoped_release release;
            return normalizer.snapshot(now_wall_ns, now_monotonic_ns);
          },
          py::arg("now_wall_ns"), py::arg("now_monotonic_ns"),
          "State and freshness of every instrument as of the caller's clocks, as JSON.")
      .def("retain_symbols", &markettwin::live::LiveNormalizer::retain_symbols,
           py::arg("symbols_json"), "Forget every instrument not named in this JSON array.")
      .def_property_readonly("last_batch_elapsed_ns",
                             &markettwin::live::LiveNormalizer::last_batch_elapsed_ns);

  module.def(
      "calibration_observed_facts",
      [](const std::string& session, std::int64_t start_ns, std::int64_t end_ns) {
        const py::gil_scoped_release release;
        return markettwin::calibration::observed_facts_json(session, start_ns, end_ns);
      },
      py::arg("session_directory"), py::arg("start_ns"), py::arg("end_ns"),
      "Stylized facts of a historical session's events inside [start_ns, end_ns), as JSON.");
  module.def(
      "calibration_synthetic_facts",
      [](const std::string& config_text) {
        const py::gil_scoped_release release;
        return markettwin::calibration::synthetic_facts_json(config_text);
      },
      py::arg("config_text"),
      "Run a config in memory and return the stylized facts of its L1 projection, as JSON.");
  module.def(
      "calibration_targets",
      [](const std::string& facts_array) {
        const py::gil_scoped_release release;
        return markettwin::calibration::targets_from_facts_json(facts_array);
      },
      py::arg("facts_array_json"), "Mean and floored scale of each fact across sessions.");
  module.def(
      "calibration_score",
      [](const std::string& target, const std::string& scale, const std::string& facts_array) {
        const py::gil_scoped_release release;
        return markettwin::calibration::score_from_json(target, scale, facts_array);
      },
      py::arg("target_json"), py::arg("scale_json"), py::arg("facts_array_json"),
      "Score one candidate's per-seed facts against target means and scales, as JSON.");

  module.def("canonicalize_experiment", &canonicalize_experiment, py::arg("config_text"),
             py::arg("base_directory") = std::string{},
             "Parse and validate an experiment config. Returns its canonical JSON.");
  module.def("experiment_cell_config", &experiment_cell_config, py::arg("config_text"),
             py::arg("base_directory"), py::arg("variant"), py::arg("seed"),
             "The exact canonical run config of one (variant, seed) cell.");

  py::class_<ExperimentProgress, std::shared_ptr<ExperimentProgress>>(module, "ExperimentProgress")
      .def(py::init<>())
      .def_property_readonly("total", &ExperimentProgress::total)
      .def_property_readonly("completed", &ExperimentProgress::completed)
      .def_property_readonly("failed", &ExperimentProgress::failed)
      .def_property_readonly("cancel_requested", &ExperimentProgress::cancel_requested)
      .def_property_readonly("current_run_id", &ExperimentProgress::current_run_id)
      .def("request_cancel", &ExperimentProgress::request_cancel);

  module.def("run_experiment", &run_experiment, py::arg("config_text"),
             py::arg("base_directory") = std::string{}, py::arg("output_directory") = std::string{},
             py::arg("progress") = std::shared_ptr<ExperimentProgress>{},
             "Run every cell of an experiment. Returns (results JSON, paired values JSON).");
}
