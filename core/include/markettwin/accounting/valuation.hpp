#ifndef MARKETTWIN_ACCOUNTING_VALUATION_HPP
#define MARKETTWIN_ACCOUNTING_VALUATION_HPP

#include "markettwin/accounting/ledger.hpp"

#include <optional>
#include <string_view>

namespace markettwin {

// Where a mark came from, in order of preference. Marks are market prices: the simulated
// fundamental value is never one.
//   Midpoint   both a bid and an ask are resting: (bid + ask) / 2
//   LastTrade  the book is one-sided or empty, and at least one trade has printed
//   Reference  nothing has traded yet: the scenario's public reference price
//   None       none of the above exists; open positions cannot be valued
// A lone bid or ask is deliberately not a mark: one side of a book is an offer to trade,
// not a price at which the position could be closed.
enum class MarkSource { None, Midpoint, LastTrade, Reference };

struct Mark {
  MarkSource source = MarkSource::None;
  std::int64_t twice_price_ticks = 0; // twice the price, so a half-tick midpoint is exact
  bool operator==(const Mark&) const = default;
};

[[nodiscard]] Mark select_mark(std::optional<PriceTicks> best_bid,
                               std::optional<PriceTicks> best_ask,
                               std::optional<PriceTicks> last_trade,
                               std::optional<PriceTicks> reference);
[[nodiscard]] std::string_view mark_source_name(MarkSource source);

// Mark-to-market figures for one account, all in minor units.
//   market_value   = position * mark                (rounded half away from zero)
//   unrealized_pnl = market_value - cost_basis
//   equity         = cash + market_value
//   total_pnl      = equity - (opening cash + opening basis)
//                  = realized_pnl - fees_paid + unrealized_pnl, exactly
struct Valuation {
  Money market_value;
  Money unrealized_pnl;
  Money equity;
  Money total_pnl;
  bool operator==(const Valuation&) const = default;
};

// A flat account needs no mark. An open position with no mark has no valuation, and the
// result is empty rather than a number.
[[nodiscard]] std::optional<Valuation> value_account(const Account& account, const Mark& mark,
                                                     std::uint64_t tick_size_minor);

// Running peak and drawdown of an equity series.
//   max_drawdown = max over observations of (highest equity so far - equity), >= 0
// It is defined on the observations it is given, so it inherits their sampling schedule.
class EquityTracker {
public:
  void observe(Money equity);
  [[nodiscard]] std::uint64_t observations() const noexcept { return observations_; }
  [[nodiscard]] std::optional<Money> peak() const noexcept;
  [[nodiscard]] std::optional<Money> max_drawdown() const noexcept;

private:
  std::uint64_t observations_ = 0;
  Money peak_;
  Money max_drawdown_;
};

} // namespace markettwin
#endif // MARKETTWIN_ACCOUNTING_VALUATION_HPP
