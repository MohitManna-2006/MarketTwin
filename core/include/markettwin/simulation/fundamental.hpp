#ifndef MARKETTWIN_SIMULATION_FUNDAMENTAL_HPP
#define MARKETTWIN_SIMULATION_FUNDAMENTAL_HPP

#include "markettwin/simulation/scenario.hpp"

namespace markettwin {

// The exogenous latent value. Integer-only, so a seed gives one path on every toolchain.
class FundamentalProcess {
public:
  static constexpr std::int64_t kMinimum = kMilliticks; // one tick
  static constexpr std::int64_t kMaximum = 4'000'000'000'000;

  FundamentalProcess(const FundamentalConfig& config, std::uint64_t seed);
  void step();
  void jump(std::int64_t delta_milliticks) noexcept;
  void set_volatility_ppm(std::uint64_t volatility_ppm) noexcept {
    volatility_ppm_ = volatility_ppm;
  }

  [[nodiscard]] std::int64_t value() const noexcept { return value_; }
  [[nodiscard]] std::int64_t anchor() const noexcept { return anchor_; }
  [[nodiscard]] std::uint64_t volatility_milliticks() const noexcept;
  [[nodiscard]] std::uint64_t updates() const noexcept { return updates_; }
  [[nodiscard]] const Rng& rng() const noexcept { return rng_; }

private:
  FundamentalConfig config_;
  Rng rng_;
  std::int64_t value_;
  std::int64_t anchor_;
  std::uint64_t volatility_ppm_ = kPpm;
  std::uint64_t updates_ = 0;
};

} // namespace markettwin
#endif // MARKETTWIN_SIMULATION_FUNDAMENTAL_HPP
