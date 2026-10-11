#ifndef MARKETTWIN_ACCOUNTING_FEES_HPP
#define MARKETTWIN_ACCOUNTING_FEES_HPP

#include "markettwin/accounting/money.hpp"

namespace markettwin {

inline constexpr std::int64_t kMaxFeePpm = 1'000'000;

// Proportional fees on traded notional, in parts per million. A negative rate is a
// rebate. Every fee is paid to (or by) the ledger's fee sink, so cash is conserved
// across the agents and the sink together.
struct FeeSchedule {
  std::int64_t maker_ppm = 0;
  std::int64_t taker_ppm = 0;
  bool operator==(const FeeSchedule&) const = default;
};

// notional * rate_ppm / 1e6, rounded to the nearest minor unit with ties away from zero.
// Each fill is charged on its own notional; fees are never netted before rounding.
[[nodiscard]] Money fee_for(Money notional, std::int64_t rate_ppm);

} // namespace markettwin
#endif // MARKETTWIN_ACCOUNTING_FEES_HPP
