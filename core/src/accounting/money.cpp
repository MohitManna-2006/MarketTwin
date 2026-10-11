#include "markettwin/accounting/money.hpp"

#include <limits>

namespace markettwin {
namespace {

constexpr Int128 kMin64 = std::numeric_limits<std::int64_t>::min();
constexpr Int128 kMax64 = std::numeric_limits<std::int64_t>::max();

[[noreturn]] void overflow(const char* what) {
  throw RunError{"AccountingOverflow", what};
}

// magnitude * multiplier / 2, ties away from zero, with the sign restored afterwards.
// The magnitude is at most 2^126, so the product is checked by division before it is
// formed and nothing here can wrap.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a signed total, then its unit size.
Money scale_half(Int128 value, std::uint64_t multiplier) {
  const bool negative = value < 0;
  const UInt128 magnitude = negative ? -static_cast<UInt128>(value) : static_cast<UInt128>(value);
  if (magnitude != 0 && multiplier > ~UInt128{0} / magnitude) {
    overflow("marked value exceeds 64-bit money");
  }
  const UInt128 product = magnitude * multiplier;
  const UInt128 rounded = (product / 2) + (product % 2);
  if (rounded > static_cast<UInt128>(kMax64)) {
    overflow("marked value exceeds 64-bit money");
  }
  const auto result = static_cast<std::int64_t>(rounded);
  return Money{negative ? -result : result};
}

} // namespace

std::int64_t narrow(Int128 value) {
  if (value < kMin64 || value > kMax64) {
    overflow("value exceeds 64-bit range");
  }
  return static_cast<std::int64_t>(value);
}

std::int64_t checked_add(std::int64_t left, std::int64_t right) {
  return narrow(Int128{left} + Int128{right});
}

Money checked_add(Money left, Money right) {
  return Money{checked_add(left.value(), right.value())};
}

Money checked_subtract(Money left, Money right) {
  return Money{narrow(Int128{left.value()} - Int128{right.value()})};
}

Money checked_negate(Money value) {
  return Money{narrow(-Int128{value.value()})};
}

Int128 divide_round(Int128 numerator, Int128 denominator) {
  if (denominator <= 0) {
    throw RunError{"InvalidArgument", "rounded division needs a positive denominator"};
  }
  const Int128 quotient = numerator / denominator;
  const Int128 remainder = numerator % denominator; // carries the sign of the numerator
  const Int128 magnitude = remainder < 0 ? -remainder : remainder;
  if (magnitude >= denominator - magnitude) {
    return numerator < 0 ? quotient - 1 : quotient + 1;
  }
  return quotient;
}

UInt128 integer_sqrt(UInt128 value) noexcept {
  // Digit-by-digit square root: exact, branch-simple, and free of floating point.
  UInt128 root = 0;
  UInt128 bit = UInt128{1} << 126U;
  while (bit > value) {
    bit >>= 2U;
  }
  while (bit != 0) {
    if (value >= root + bit) {
      value -= root + bit;
      root = (root >> 1U) + bit;
    } else {
      root >>= 1U;
    }
    bit >>= 2U;
  }
  return root;
}

Money notional(PriceTicks price, Quantity quantity, std::uint64_t tick_size_minor) {
  // |price| < 2^63 and quantity < 2^64, so the first product fits in 128 bits. If it is
  // already outside 64 bits the final value is too, because the tick size is at least 1.
  const Int128 shares_value = Int128{price.value()} * static_cast<Int128>(quantity.value());
  return Money{narrow(Int128{narrow(shares_value)} * static_cast<Int128>(tick_size_minor))};
}

Money prorate(Money basis, std::uint64_t part, std::uint64_t whole) {
  if (part == 0 || part > whole) {
    throw RunError{"InvalidArgument", "prorated part must be within (0, whole]"};
  }
  // |basis| < 2^63 and part < 2^64: the product fits. The quotient is no larger than
  // the basis, so it always narrows.
  const Int128 product = Int128{basis.value()} * static_cast<Int128>(part);
  return Money{narrow(product / static_cast<Int128>(whole))};
}

Money mark_value(std::int64_t position, std::int64_t twice_price_ticks,
                 std::uint64_t tick_size_minor) {
  return scale_half(Int128{position} * Int128{twice_price_ticks}, tick_size_minor);
}

Money half_tick_shares_to_money(Int128 half_tick_shares, std::uint64_t tick_size_minor) {
  constexpr auto kLimit = static_cast<Int128>(UInt128{1} << 126U);
  if (half_tick_shares <= -kLimit || half_tick_shares >= kLimit) {
    overflow("half-tick share total exceeds the supported range");
  }
  return scale_half(half_tick_shares, tick_size_minor);
}

} // namespace markettwin
