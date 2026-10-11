#include "markettwin/simulation/fundamental.hpp"

#include <algorithm>

namespace markettwin {

FundamentalProcess::FundamentalProcess(const FundamentalConfig& config, std::uint64_t seed)
    : config_(config), rng_(seed, RngStream::Fundamental), value_(config.initial_milliticks),
      anchor_(config.initial_milliticks) {}

std::uint64_t FundamentalProcess::volatility_milliticks() const noexcept {
  return (config_.volatility_milliticks * volatility_ppm_) / kPpm;
}

void FundamentalProcess::step() {
  // Truncating division is symmetric around zero, so neither term adds drift.
  const auto pull = ((anchor_ - value_) * to_signed(config_.mean_reversion_ppm)) / to_signed(kPpm);
  const auto shock = (rng_.normal_q16() * to_signed(volatility_milliticks())) / 65'536;
  value_ = std::clamp(value_ + pull + shock, kMinimum, kMaximum);
  ++updates_;
}

void FundamentalProcess::jump(std::int64_t delta_milliticks) noexcept {
  anchor_ = std::clamp(anchor_ + delta_milliticks, kMinimum, kMaximum);
  value_ = std::clamp(value_ + delta_milliticks, kMinimum, kMaximum);
}

} // namespace markettwin
