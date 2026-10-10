#pragma once

#include <compare>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace markettwin {

template <typename Tag, typename Representation> class StrongInteger {
public:
  constexpr StrongInteger() = default;
  explicit constexpr StrongInteger(Representation value) : value_(value) {}
  [[nodiscard]] constexpr Representation value() const noexcept { return value_; }
  auto operator<=>(const StrongInteger&) const = default;

private:
  Representation value_{};
};

using PriceTicks = StrongInteger<struct PriceTicksTag, std::int64_t>;
using Quantity = StrongInteger<struct QuantityTag, std::uint64_t>;
using SimTime = StrongInteger<struct SimTimeTag, std::uint64_t>;
using Sequence = StrongInteger<struct SequenceTag, std::uint64_t>;

[[nodiscard]] inline Quantity checked_add(Quantity left, Quantity right) {
  if (right.value() > std::numeric_limits<std::uint64_t>::max() - left.value()) {
    throw std::overflow_error("quantity addition overflow");
  }
  return Quantity{left.value() + right.value()};
}

[[nodiscard]] inline Quantity checked_subtract(Quantity left, Quantity right) {
  if (right > left) {
    throw std::underflow_error("quantity subtraction underflow");
  }
  return Quantity{left.value() - right.value()};
}

} // namespace markettwin
