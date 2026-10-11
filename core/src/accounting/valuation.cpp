#include "markettwin/accounting/valuation.hpp"

#include <algorithm>
#include <array>

namespace markettwin {

Mark select_mark(std::optional<PriceTicks> best_bid, std::optional<PriceTicks> best_ask,
                 std::optional<PriceTicks> last_trade, std::optional<PriceTicks> reference) {
  const auto twice = [](PriceTicks price) { return checked_add(price.value(), price.value()); };
  if (best_bid && best_ask) {
    return {
        .source = MarkSource::Midpoint,
        .twice_price_ticks = checked_add(best_bid->value(), best_ask->value()),
    };
  }
  if (last_trade) {
    return {.source = MarkSource::LastTrade, .twice_price_ticks = twice(*last_trade)};
  }
  if (reference) {
    return {.source = MarkSource::Reference, .twice_price_ticks = twice(*reference)};
  }
  return {};
}

std::string_view mark_source_name(MarkSource source) {
  constexpr std::array<std::string_view, 4> kNames{"None", "Midpoint", "LastTrade", "Reference"};
  return kNames.at(static_cast<std::size_t>(source));
}

std::optional<Valuation> value_account(const Account& account, const Mark& mark,
                                       std::uint64_t tick_size_minor) {
  if (account.position != 0 && mark.source == MarkSource::None) {
    return std::nullopt;
  }
  const auto market_value =
      account.position == 0 ? Money{}
                            : mark_value(account.position, mark.twice_price_ticks, tick_size_minor);
  const auto equity = checked_add(account.cash, market_value);
  return Valuation{
      .market_value = market_value,
      .unrealized_pnl = checked_subtract(market_value, account.cost_basis),
      .equity = equity,
      .total_pnl =
          checked_subtract(equity, checked_add(account.initial_cash, account.initial_cost_basis)),
  };
}

void EquityTracker::observe(Money equity) {
  if (observations_ == 0) {
    peak_ = equity;
  }
  peak_ = std::max(peak_, equity);
  max_drawdown_ = std::max(max_drawdown_, checked_subtract(peak_, equity));
  ++observations_;
}

std::optional<Money> EquityTracker::peak() const noexcept {
  return observations_ == 0 ? std::nullopt : std::optional{peak_};
}

std::optional<Money> EquityTracker::max_drawdown() const noexcept {
  return observations_ == 0 ? std::nullopt : std::optional{max_drawdown_};
}

} // namespace markettwin
