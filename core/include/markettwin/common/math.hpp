#ifndef MARKETTWIN_COMMON_MATH_HPP
#define MARKETTWIN_COMMON_MATH_HPP

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace markettwin {

inline constexpr std::uint64_t kPpm = 1'000'000;
// Sub-tick resolution for latent values and signals. Orders are always whole ticks.
inline constexpr std::int64_t kMilliticks = 1'000;

[[nodiscard]] constexpr std::int64_t to_signed(std::uint64_t value) {
  if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    throw std::overflow_error("unsigned value exceeds signed 64-bit range");
  }
  return static_cast<std::int64_t>(value);
}

// Integer division rounds toward zero; quotes need an explicit direction instead.
[[nodiscard]] constexpr std::int64_t floor_div(std::int64_t value, std::int64_t divisor) noexcept {
  const auto quotient = value / divisor;
  return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
}

[[nodiscard]] constexpr std::int64_t ceil_div(std::int64_t value, std::int64_t divisor) noexcept {
  const auto quotient = value / divisor;
  return (value % divisor != 0 && (value < 0) == (divisor < 0)) ? quotient + 1 : quotient;
}

} // namespace markettwin
#endif // MARKETTWIN_COMMON_MATH_HPP
