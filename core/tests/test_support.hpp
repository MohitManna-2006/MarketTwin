#pragma once

#include "markettwin/simulation/kernel.hpp"
#include "markettwin/simulation/simulation.hpp"

#include <fstream>
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>

namespace markettwin::test {

class TempDirectory {
public:
  TempDirectory() {
    std::random_device random;
    for (int attempt = 0; attempt < 10; ++attempt) {
      path_ = std::filesystem::temp_directory_path() /
              ("markettwin-test-" + std::to_string(random()) + "-" + std::to_string(random()));
      if (std::filesystem::create_directory(path_)) {
        return;
      }
    }
    throw std::runtime_error("could not allocate test directory");
  }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;
  TempDirectory(TempDirectory&&) = delete;
  TempDirectory& operator=(TempDirectory&&) = delete;
  [[nodiscard]] std::filesystem::path run() const { return path_ / "run"; }
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
  std::filesystem::path path_;
};

inline std::string read(const std::filesystem::path& path) {
  std::ifstream input{path, std::ios::binary};
  input.exceptions(std::ios::badbit);
  if (!input) {
    throw std::runtime_error("cannot read test file");
  }
  std::ostringstream output;
  output << input.rdbuf();
  return output.str();
}
inline void write(const std::filesystem::path& path, std::string_view bytes) {
  std::ofstream output{path, std::ios::binary};
  output.exceptions(std::ios::badbit | std::ios::failbit);
  output << bytes;
  output.close();
}
inline Order order(std::uint64_t id, Side side, std::uint64_t owner = 1,
                   std::uint64_t quantity = 5) {
  return {.id = OrderId{id},
          .owner = AgentId{owner},
          .symbol = SymbolId{1},
          .side = side,
          .type = OrderType::Limit,
          .limit_price = PriceTicks{100},
          .original_quantity = Quantity{quantity},
          .remaining_quantity = Quantity{quantity},
          .tif = TimeInForce::Gtc,
          .arrival_time = {},
          .arrival_sequence = {}};
}
inline ScheduledCommand at(std::uint64_t timestamp, const Command& command) {
  return {.timestamp = SimTime{timestamp}, .command = command};
}
inline CancelCommand cancel(std::uint64_t order_id, std::uint64_t owner) {
  return {.order_id = OrderId{order_id}, .owner = AgentId{owner}};
}
inline RunConfig fixture() {
  RunConfig config;
  config.run_id = "fixture";
  config.seed = 42;
  config.commands = {at(20, SnapshotCommand{.depth = 2}), at(0, order(1, Side::Sell)),
                     at(1, order(2, Side::Buy, 2, 3)), at(2, cancel(1, 2)), at(3, cancel(1, 1))};
  return config;
}

// Keeps every record of an in-memory run so a test can inspect the trace it produced.
class CaptureSink final : public RecordSink {
public:
  void append(TraceChannel channel, std::string_view record, Sequence /*sequence*/) override {
    (channel == TraceChannel::Commands ? commands : trace).emplace_back(record);
  }
  // Parsed records of one kind, in trace order.
  [[nodiscard]] static std::vector<nlohmann::json> of_kind(const std::vector<std::string>& lines,
                                                           std::string_view kind) {
    // Canonical records spell the envelope kind exactly once, so match before parsing.
    const auto marker = "\"kind\":\"" + std::string{kind} + "\",\"payload\"";
    std::vector<nlohmann::json> result;
    for (const auto& line : lines) {
      if (line.find(marker) != std::string::npos) {
        result.push_back(nlohmann::json::parse(line));
      }
    }
    return result;
  }
  std::vector<std::string> commands;
  std::vector<std::string> trace;
};

inline constexpr std::uint64_t kMillisecond = 1'000'000;
inline constexpr std::uint64_t kSecond = 1'000'000'000;

inline AgentConfig agent(std::uint64_t id, const PolicyConfig& policy,
                         std::uint64_t interval = kSecond) {
  return {
      .id = AgentId{id},
      .wakeup = {.first = SimTime{0},
                 .interval = SimTime{interval},
                 .distribution = WakeupDistribution::Fixed},
      .latency = {.order_entry = SimTime{kMillisecond},
                  .order_entry_jitter = SimTime{0},
                  .report = SimTime{kMillisecond},
                  .market_data = SimTime{kMillisecond}},
      .limits = {.max_position = 10'000,
                 .max_order_quantity = Quantity{100},
                 .max_open_orders = 16,
                 .max_messages = 1'000'000},
      .policy = policy,
  };
}

// Two makers, a value trader, a momentum trader, and two noise traders for one minute.
// The value trader is sized to dominate the noise so prices track the fundamental.
inline RunConfig scenario_fixture() {
  RunConfig config;
  config.run_id = "scenario";
  config.seed = 7;
  config.exchange.agents = {AgentId{1}, AgentId{2}, AgentId{3}, AgentId{4}, AgentId{5}, AgentId{6}};
  config.limits = {.horizon = SimTime{60 * kSecond}, .max_events = 5'000'000, .max_queue = 100'000};
  ScenarioConfig scenario;
  scenario.sampling = SamplingConfig{.interval = SimTime{kSecond}, .depth = 5};
  scenario.agents = {
      agent(1, MarketMakerParams{.half_spread_ticks = 2, .quote_size = Quantity{100}, .levels = 2},
            500 * kMillisecond),
      agent(2,
            InventoryMarketMakerParams{
                .quoting = {.half_spread_ticks = 2, .quote_size = Quantity{100}, .levels = 2}},
            500 * kMillisecond),
      agent(3, ValueParams{.threshold_milliticks = 2'000, .order_size = Quantity{50}},
            500 * kMillisecond),
      agent(4, MomentumParams{.short_window = 2, .long_window = 5, .threshold_milliticks = 500}),
      agent(5, NoiseParams{}, kSecond),
      agent(6, NoiseParams{}, 1'500 * kMillisecond),
  };
  scenario.agents.at(4).wakeup.distribution = WakeupDistribution::Exponential;
  scenario.agents.at(5).wakeup.distribution = WakeupDistribution::Exponential;
  config.scenario = std::move(scenario);
  return config;
}

// Checked access to the optional scenario parts, so a test fails loudly instead of
// dereferencing an empty optional.
inline ScenarioConfig& scenario(RunConfig& config) {
  if (!config.scenario) {
    throw std::logic_error("config has no scenario");
  }
  return *config.scenario;
}
inline const ScenarioSummary& scenario(const RunSummary& summary) {
  if (!summary.scenario) {
    throw std::logic_error("summary has no scenario");
  }
  return *summary.scenario;
}

struct CapturedRun {
  RunSummary summary;
  CaptureSink records;
};

inline CapturedRun capture(const RunConfig& config) {
  CapturedRun run;
  run.summary = run_kernel(config, run.records);
  return run;
}

} // namespace markettwin::test
