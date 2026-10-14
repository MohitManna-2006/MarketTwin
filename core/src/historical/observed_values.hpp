#ifndef MARKETTWIN_SRC_HISTORICAL_OBSERVED_VALUES_HPP
#define MARKETTWIN_SRC_HISTORICAL_OBSERVED_VALUES_HPP

#include <cstdint>
#include <string>
#include <string_view>

// Checked conversions shared by every observed-market path: the Phase 4A historical
// importer and the Phase 4B live normalizer. Both must agree exactly on what a price and
// a timestamp are, so there is one implementation. Every failure is a RunError with code
// InvalidHistoricalData.
namespace markettwin::historical::detail {

inline constexpr std::int64_t kPriceScale = 1'000'000;

// Unsigned decimal text to exact millionths of a US dollar. At most six fractional
// digits; no sign, exponent, or whitespace; overflow is rejected. `name` names the
// source field in the error.
[[nodiscard]] std::int64_t parse_price_micro_usd(std::string_view text, const std::string& name,
                                                 bool allow_zero);

struct UtcTimestamp {
  std::int64_t nanoseconds = 0;
  // Fractional digits present in the source text, 0 through 9.
  int precision_digits = 0;
};

// RFC3339 with an explicit Z and at most nine fractional digits. Offsets are not
// guessed and precision absent from the source is not invented.
[[nodiscard]] UtcTimestamp parse_utc_timestamp(std::string_view text);

// Upper-case letters and dots, 1 to 12 characters.
[[nodiscard]] bool valid_equity_symbol(std::string_view symbol) noexcept;

} // namespace markettwin::historical::detail

#endif
