#include "markettwin/calibration/calibration.hpp"
#include "markettwin/common/run_error.hpp"
#include "markettwin/common/version.hpp"
#include "markettwin/experiment/experiment.hpp"
#include "markettwin/historical/historical.hpp"
#include "markettwin/simulation/simulation.hpp"
#include "markettwin/telemetry/replay.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace {
void print_usage(std::ostream& output) {
  output << "Usage: markettwin --version | --help\n"
         << "       markettwin run --config CONFIG.json --output NEW_DIRECTORY\n"
         << "       markettwin replay --input RUN_DIRECTORY\n"
         << "       markettwin experiment --config EXPERIMENT.json --output NEW_DIRECTORY\n";
  output << "       markettwin historical-import --input RAW.jsonl --metadata META.json --output "
            "NEW_DIRECTORY\n"
         << "       markettwin historical-verify --input SESSION_DIRECTORY\n"
         << "       markettwin calibration-facts --config CONFIG.json\n"
         << "       markettwin calibration-facts --session SESSION_DIRECTORY --start UTC_NS --end "
            "UTC_NS\n";
}

// The calibration subcommands, or nothing when the arguments name another command.
std::optional<int> calibration_command(int argc, char* const* argv) {
  if (argc == 4 && std::string_view{argv[1]} == "calibration-facts" &&
      std::string_view{argv[2]} == "--config") {
    const std::ifstream config{argv[3]};
    if (!config) {
      throw markettwin::RunError{"IoError", "cannot read run config"};
    }
    std::ostringstream text;
    text << config.rdbuf();
    std::cout << markettwin::calibration::synthetic_facts_json(text.str()) << '\n';
    return 0;
  }
  if (argc == 8 && std::string_view{argv[1]} == "calibration-facts" &&
      std::string_view{argv[2]} == "--session" && std::string_view{argv[4]} == "--start" &&
      std::string_view{argv[6]} == "--end") {
    std::cout << markettwin::calibration::observed_facts_json(argv[3], std::stoll(argv[5]),
                                                              std::stoll(argv[7]))
              << '\n';
    return 0;
  }
  return std::nullopt;
}

int execute(int argc, char* const* argv) {
  if (argc == 2 && std::string_view{argv[1]} == "--version") {
    std::cout << "MarketTwin " << markettwin::version() << '\n';
    return 0;
  }
  if (argc == 2 && (std::string_view{argv[1]} == "--help" || std::string_view{argv[1]} == "-h")) {
    print_usage(std::cout);
    return 0;
  }
  if (argc == 6 && std::string_view{argv[1]} == "run" && std::string_view{argv[2]} == "--config" &&
      std::string_view{argv[4]} == "--output") {
    markettwin::Simulation simulation{markettwin::load_config(argv[3]), argv[5]};
    std::cout << markettwin::summary_json(simulation.run()) << '\n';
    return 0;
  }
  if (argc == 4 && std::string_view{argv[1]} == "replay" &&
      std::string_view{argv[2]} == "--input") {
    const auto result = markettwin::verify_replay(argv[3]);
    std::cout << markettwin::replay_result_json(result) << '\n';
    return result.verified ? 0 : 1;
  }
  if (argc == 6 && std::string_view{argv[1]} == "experiment" &&
      std::string_view{argv[2]} == "--config" && std::string_view{argv[4]} == "--output") {
    const auto config = markettwin::load_experiment(argv[3]);
    const auto result = markettwin::run_experiment(config, argv[5]);
    std::cout << markettwin::experiment_report(config, result);
    // Results are written either way; a failed run still makes the command fail.
    const bool complete = std::ranges::all_of(result.runs, &markettwin::RunOutcome::completed);
    return complete ? 0 : 1;
  }
  if (argc == 8 && std::string_view{argv[1]} == "historical-import" &&
      std::string_view{argv[2]} == "--input" && std::string_view{argv[4]} == "--metadata" &&
      std::string_view{argv[6]} == "--output") {
    const std::ifstream metadata{argv[5]};
    if (!metadata) {
      throw markettwin::RunError{"IoError", "cannot read historical metadata"};
    }
    // Read through rdbuf rather than istreambuf_iterator: GCC 13 at -O3 reports a false
    // -Wnull-dereference inside the inlined iterator.
    std::ostringstream text;
    text << metadata.rdbuf();
    std::cout << markettwin::historical::import_jsonl(argv[3], argv[7], text.str()) << '\n';
    return 0;
  }
  if (argc == 4 && std::string_view{argv[1]} == "historical-verify" &&
      std::string_view{argv[2]} == "--input") {
    const auto result = markettwin::historical::verify(argv[3]);
    std::cout << result << '\n';
    return nlohmann::json::parse(result).value("verified", false) ? 0 : 1;
  }
  if (const auto status = calibration_command(argc, argv)) {
    return *status;
  }
  print_usage(std::cerr);
  return 2;
}
} // namespace

int main(int argc, char* const* argv) {
  try {
    return execute(argc, argv);
  } catch (const markettwin::RunError& error) {
    std::cerr << error.code() << " at sequence " << error.sequence().value() << ": " << error.what()
              << '\n';
  } catch (const std::exception& error) {
    std::cerr << "RunFailed: " << error.what() << '\n';
  }
  return 1;
}
