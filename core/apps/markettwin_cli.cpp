#include "markettwin/common/run_error.hpp"
#include "markettwin/common/version.hpp"
#include "markettwin/experiment/experiment.hpp"
#include "markettwin/simulation/simulation.hpp"
#include "markettwin/telemetry/replay.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>

namespace {
void print_usage(std::ostream& output) {
  output << "Usage: markettwin --version | --help\n"
         << "       markettwin run --config CONFIG.json --output NEW_DIRECTORY\n"
         << "       markettwin replay --input RUN_DIRECTORY\n"
         << "       markettwin experiment --config EXPERIMENT.json --output NEW_DIRECTORY\n";
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
