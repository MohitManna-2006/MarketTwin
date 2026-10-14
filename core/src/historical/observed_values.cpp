#include "observed_values.hpp"

#include "markettwin/common/run_error.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <limits>
#include <string>

namespace markettwin::historical::detail {
namespace {

[[noreturn]] void invalid(const std::string& message) {
  throw RunError{"InvalidHistoricalData", message};
}

bool all_digits(std::string_view text) noexcept {
  return !text.empty() && std::ranges::all_of(text, [](unsigned char character) {
    return character >= '0' && character <= '9';
  });
}

std::int64_t integer(std::string_view text, std::string_view field) {
  std::int64_t number = 0;
  const auto* const last = text.data() + text.size();
  const auto [end, error] = std::from_chars(text.data(), last, number);
  if (error != std::errc{} || end != last) {
    invalid("invalid or overflowing " + std::string{field});
  }
  return number;
}

} // namespace

std::int64_t parse_price_micro_usd(std::string_view text, std::string_view field, bool allow_zero) {
  const std::string name{field};
  if (text.empty() || text.front() == '-' || text.front() == '+') {
    invalid("invalid price: " + name);
  }
  const auto point = text.find('.');
  if (point != std::string_view::npos &&
      (text.find('.', point + 1) != std::string_view::npos || text.size() - point - 1 > 6 ||
       point + 1 == text.size())) {
    invalid("price exceeds six decimal places: " + name);
  }
  const auto whole = integer(text.substr(0, point), field);
  if (whole > std::numeric_limits<std::int64_t>::max() / kPriceScale) {
    invalid("price overflow: " + name);
  }
  std::int64_t fractional = 0;
  if (point != std::string_view::npos) {
    const auto decimals = text.substr(point + 1);
    fractional = integer(decimals, field);
    for (std::size_t index = decimals.size(); index < 6; ++index) {
      fractional *= 10;
    }
  }
  if (whole * kPriceScale > std::numeric_limits<std::int64_t>::max() - fractional) {
    invalid("price overflow: " + name);
  }
  const auto result = (whole * kPriceScale) + fractional;
  if (result == 0 && !allow_zero) {
    invalid("zero price: " + name);
  }
  return result;
}

UtcTimestamp parse_utc_timestamp(std::string_view text) {
  // YYYY-MM-DDTHH:MM:SS[.f{1,9}]Z, parsed by position: this runs once per live record.
  constexpr std::size_t kSecondsLength = 19;
  const auto shaped = text.size() > kSecondsLength && text.back() == 'Z' && text.at(4) == '-' &&
                      text.at(7) == '-' && text.at(10) == 'T' && text.at(13) == ':' &&
                      text.at(16) == ':' && all_digits(text.substr(0, 4)) &&
                      all_digits(text.substr(5, 2)) && all_digits(text.substr(8, 2)) &&
                      all_digits(text.substr(11, 2)) && all_digits(text.substr(14, 2)) &&
                      all_digits(text.substr(17, 2));
  const auto fraction_text = shaped && text.size() > kSecondsLength + 1
                                 ? text.substr(kSecondsLength + 1, text.size() - kSecondsLength - 2)
                                 : std::string_view{};
  if (!shaped || (text.size() > kSecondsLength + 1 &&
                  (text.at(kSecondsLength) != '.' || fraction_text.size() > 9 ||
                   !all_digits(fraction_text)))) {
    invalid("timestamp must be RFC3339 UTC with at most nine fractional digits");
  }
  const auto number = [&](std::size_t offset, std::size_t length) {
    return static_cast<int>(integer(text.substr(offset, length), "time"));
  };
  const std::chrono::year_month_day date{std::chrono::year{number(0, 4)},
                                         std::chrono::month{static_cast<unsigned>(number(5, 2))},
                                         std::chrono::day{static_cast<unsigned>(number(8, 2))}};
  const auto hour = number(11, 2);
  const auto minute = number(14, 2);
  const auto second = number(17, 2);
  if (!date.ok() || hour > 23 || minute > 59 || second > 59) {
    invalid("timestamp calendar fields are invalid");
  }
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::sys_days{date}.time_since_epoch())
                           .count() +
                       (static_cast<std::int64_t>(hour) * 3600) +
                       (static_cast<std::int64_t>(minute) * 60) + static_cast<std::int64_t>(second);
  if (seconds < 0 || seconds > std::numeric_limits<std::int64_t>::max() / 1'000'000'000) {
    invalid("timestamp is outside supported UTC nanoseconds");
  }
  UtcTimestamp result;
  std::int64_t fraction = 0;
  if (!fraction_text.empty()) {
    result.precision_digits = static_cast<int>(fraction_text.size());
    fraction = integer(fraction_text, "fractional timestamp");
    for (int index = result.precision_digits; index < 9; ++index) {
      fraction *= 10;
    }
  }
  result.nanoseconds = (seconds * 1'000'000'000) + fraction;
  return result;
}

bool valid_equity_symbol(std::string_view symbol) noexcept {
  return !symbol.empty() && symbol.size() <= 12 &&
         std::ranges::all_of(symbol, [](unsigned char character) {
           return (character >= 'A' && character <= 'Z') || character == '.';
         });
}

} // namespace markettwin::historical::detail
