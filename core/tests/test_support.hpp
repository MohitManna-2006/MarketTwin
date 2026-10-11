#pragma once

#include "markettwin/simulation/simulation.hpp"

#include <fstream>
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

} // namespace markettwin::test
