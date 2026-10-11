#ifndef MARKETTWIN_ACCOUNTING_MONEY_HPP
#define MARKETTWIN_ACCOUNTING_MONEY_HPP

#include "markettwin/common/run_error.hpp"

#include <cstdint>

namespace markettwin {

// Signed money in minor currency units (for example cents). One price tick is worth
// RunConfig::tick_size_minor minor units, so
//   notional_minor = price_ticks * quantity_shares * tick_size_minor
// is an exact integer. There are no arithmetic operators: every sum and product goes
// through a checked helper that throws RunError{"AccountingOverflow"} instead of wrapping.
using Money = StrongInteger<struct MoneyTag, std::int64_t>;

// 128-bit intermediates for products that are divided back into 64 bits. The engine is
// built with GCC and Clang, both of which provide the type on every supported target.
// NOLINTBEGIN(modernize-use-using): __extension__ is only accepted on a typedef.
__extension__ typedef __int128 Int128;
__extension__ typedef unsigned __int128 UInt128;
// NOLINTEND(modernize-use-using)

[[nodiscard]] Money checked_add(Money left, Money right);
[[nodiscard]] Money checked_subtract(Money left, Money right);
[[nodiscard]] Money checked_negate(Money value);
[[nodiscard]] std::int64_t checked_add(std::int64_t left, std::int64_t right);
// Narrows a wide intermediate, throwing when it does not fit in 64 bits.
[[nodiscard]] std::int64_t narrow(Int128 value);

// numerator / denominator rounded to the nearest integer, ties away from zero. This is
// the one rounding rule used wherever a money or metric value leaves exact arithmetic.
// The denominator must be positive.
[[nodiscard]] Int128 divide_round(Int128 numerator, Int128 denominator);

// Largest integer whose square does not exceed the value.
[[nodiscard]] UInt128 integer_sqrt(UInt128 value) noexcept;

// price * quantity * tick_size_minor, exact.
[[nodiscard]] Money notional(PriceTicks price, Quantity quantity, std::uint64_t tick_size_minor);
// basis * part / whole truncated toward zero, for 0 < part <= whole. Used to release
// cost basis when a position is partly closed; the truncation stays in the basis that
// remains, so realized plus unrealized P&L is exact whatever the rounding.
[[nodiscard]] Money prorate(Money basis, std::uint64_t part, std::uint64_t whole);
// Value of a signed position at a mark given as twice the price in ticks (so that a
// half-tick midpoint is an integer): position * twice_price * tick_size_minor / 2,
// rounded half away from zero. The only inexact step in valuation.
[[nodiscard]] Money mark_value(std::int64_t position, std::int64_t twice_price_ticks,
                               std::uint64_t tick_size_minor);
// The same conversion for a quantity already expressed in half-tick shares.
[[nodiscard]] Money half_tick_shares_to_money(Int128 half_tick_shares,
                                              std::uint64_t tick_size_minor);

} // namespace markettwin
#endif // MARKETTWIN_ACCOUNTING_MONEY_HPP
