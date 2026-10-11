#include "markettwin/accounting/ledger.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace markettwin {
namespace {

[[noreturn]] void invalid_trade(const char* what, const Trade& trade) {
  throw RunError{"InvalidTrade", what, trade.arrival_sequence};
}

std::uint64_t magnitude(std::int64_t value) noexcept {
  return value < 0 ? ~static_cast<std::uint64_t>(value) + 1 : static_cast<std::uint64_t>(value);
}

int sign(std::int64_t value) noexcept {
  return static_cast<int>(value > 0) - static_cast<int>(value < 0);
}

Quantity add_shares(Quantity total, Quantity shares) {
  if (shares.value() > std::numeric_limits<std::uint64_t>::max() - total.value()) {
    throw RunError{"AccountingOverflow", "traded share total exceeds 64 bits"};
  }
  return Quantity{total.value() + shares.value()};
}

struct FillTerms {
  Side side = Side::Buy;
  PriceTicks price;
  Quantity quantity;
  Money notional;
  Money fee;
};

// Position and basis after one fill. Works on a copy; the caller commits it.
void book_position(Account& account, const FillTerms& fill, std::uint64_t tick_size_minor) {
  const bool buy = fill.side == Side::Buy;
  const auto shares = fill.quantity.value();
  if (shares > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    throw RunError{"AccountingOverflow", "fill quantity exceeds the signed position range"};
  }
  const auto signed_shares =
      buy ? static_cast<std::int64_t>(shares) : -static_cast<std::int64_t>(shares);
  const auto signed_notional = buy ? fill.notional : checked_negate(fill.notional);
  const auto held = account.position;
  if (held == 0 || (held > 0) == buy) {
    account.cost_basis = checked_add(account.cost_basis, signed_notional);
  } else {
    const auto closing = std::min(shares, magnitude(held));
    const auto closing_notional = closing == shares
                                      ? fill.notional
                                      : notional(fill.price, Quantity{closing}, tick_size_minor);
    const auto released = closing == magnitude(held)
                              ? account.cost_basis
                              : prorate(account.cost_basis, closing, magnitude(held));
    // A sale of long shares earns its proceeds less their cost; a purchase covering a
    // short earns the short's entry value (minus a negative basis) less what it pays.
    const auto proceeds = buy ? checked_negate(closing_notional) : closing_notional;
    account.realized_pnl = checked_add(account.realized_pnl, checked_subtract(proceeds, released));
    account.cost_basis = checked_subtract(account.cost_basis, released);
    if (closing < shares) {
      const auto opening = checked_subtract(fill.notional, closing_notional);
      account.cost_basis = buy ? opening : checked_negate(opening);
    }
  }
  account.position = checked_add(held, signed_shares);
  if (account.position == std::numeric_limits<std::int64_t>::min()) {
    throw RunError{"AccountingOverflow", "position exceeds the signed 64-bit range"};
  }
}

Account booked(Account account, const FillTerms& fill, std::uint64_t tick_size_minor) {
  const bool buy = fill.side == Side::Buy;
  const auto traded = buy ? checked_negate(fill.notional) : fill.notional;
  account.cash = checked_subtract(checked_add(account.cash, traded), fill.fee);
  book_position(account, fill, tick_size_minor);
  account.fees_paid = checked_add(account.fees_paid, fill.fee);
  account.gross_notional = checked_add(account.gross_notional, fill.notional);
  (buy ? account.bought : account.sold) =
      add_shares(buy ? account.bought : account.sold, fill.quantity);
  ++account.fills;
  account.max_abs_position = std::max(account.max_abs_position, magnitude(account.position));
  account.min_cash = std::min(account.min_cash, account.cash);
  return account;
}

} // namespace

Money fee_for(Money notional, std::int64_t rate_ppm) {
  return Money{narrow(divide_round(Int128{notional.value()} * Int128{rate_ppm}, 1'000'000))};
}

Ledger::Ledger(std::vector<AccountOpening> openings, const FeeSchedule& fees,
               std::uint64_t tick_size_minor)
    : fees_(fees), tick_size_minor_(tick_size_minor) {
  const auto bad_rate = [](std::int64_t rate) { return rate < -kMaxFeePpm || rate > kMaxFeePpm; };
  if (tick_size_minor == 0 || bad_rate(fees.maker_ppm) || bad_rate(fees.taker_ppm)) {
    throw RunError{"InvalidConfig", "ledger needs a positive tick size and fees within 1e6 ppm"};
  }
  std::ranges::sort(openings, {}, &AccountOpening::agent);
  accounts_.reserve(openings.size());
  for (const auto& opening : openings) {
    const bool duplicate = !accounts_.empty() && accounts_.back().agent == opening.agent;
    const bool basis_agrees = sign(opening.cost_basis.value()) == sign(opening.position) ||
                              (opening.cost_basis == Money{} && opening.position != 0);
    if (opening.agent == AgentId{} || duplicate || !basis_agrees ||
        opening.position == std::numeric_limits<std::int64_t>::min()) {
      throw RunError{"InvalidConfig", "ledger accounts need unique nonzero agents and a cost "
                                      "basis that agrees in sign with the opening position"};
    }
    accounts_.push_back({
        .agent = opening.agent,
        .initial_cash = opening.cash,
        .initial_position = opening.position,
        .initial_cost_basis = opening.cost_basis,
        .cash = opening.cash,
        .position = opening.position,
        .cost_basis = opening.cost_basis,
        .realized_pnl = {},
        .fees_paid = {},
        .gross_notional = {},
        .bought = {},
        .sold = {},
        .fills = 0,
        .max_abs_position = magnitude(opening.position),
        .min_cash = opening.cash,
    });
  }
  static_cast<void>(totals()); // opening balances must themselves sum without overflow
}

Account* Ledger::find(AgentId agent) noexcept {
  const auto found = std::ranges::lower_bound(accounts_, agent, {}, &Account::agent);
  return found == accounts_.end() || found->agent != agent ? nullptr : &*found;
}

const Account& Ledger::account(AgentId agent) const {
  const auto found = std::ranges::lower_bound(accounts_, agent, {}, &Account::agent);
  if (found == accounts_.end() || found->agent != agent) {
    throw RunError{"UnknownAccount",
                   "no ledger account for agent " + std::to_string(agent.value())};
  }
  return *found;
}

std::optional<LedgerEntry> Ledger::apply(const Trade& trade) {
  if (trade.id == TradeId{}) {
    invalid_trade("trade ID must be positive", trade);
  }
  if (trade.id <= last_trade_) {
    return std::nullopt;
  }
  if (trade.id.value() != last_trade_.value() + 1) {
    throw RunError{"TradeSequenceGap",
                   "expected trade " + std::to_string(last_trade_.value() + 1) + " but received " +
                       std::to_string(trade.id.value()),
                   trade.arrival_sequence};
  }
  const bool maker_buys = trade.buy_order_id == trade.resting_order_id;
  const bool consistent = maker_buys ? trade.sell_order_id == trade.aggressor_order_id
                                     : trade.buy_order_id == trade.aggressor_order_id &&
                                           trade.sell_order_id == trade.resting_order_id;
  if (!consistent || trade.buy_order_id == trade.sell_order_id) {
    invalid_trade("trade order IDs do not name one buyer and one seller", trade);
  }
  if (trade.price_ticks.value() <= 0 || trade.quantity == Quantity{}) {
    invalid_trade("trade price and quantity must be positive", trade);
  }
  if (trade.maker_agent_id == trade.taker_agent_id) {
    invalid_trade("a trade needs two distinct parties", trade);
  }
  auto* maker = find(trade.maker_agent_id);
  auto* taker = find(trade.taker_agent_id);
  if (maker == nullptr || taker == nullptr) {
    throw RunError{"UnknownAccount", "trade names an agent with no ledger account",
                   trade.arrival_sequence};
  }

  const auto value = notional(trade.price_ticks, trade.quantity, tick_size_minor_);
  const auto terms = [&](bool is_maker) {
    return FillTerms{
        .side = is_maker == maker_buys ? Side::Buy : Side::Sell,
        .price = trade.price_ticks,
        .quantity = trade.quantity,
        .notional = value,
        .fee = fee_for(value, is_maker ? fees_.maker_ppm : fees_.taker_ppm),
    };
  };
  const auto maker_fill = terms(true);
  const auto taker_fill = terms(false);
  // Everything that can throw happens on copies; the three assignments below cannot.
  const auto maker_after = booked(*maker, maker_fill, tick_size_minor_);
  const auto taker_after = booked(*taker, taker_fill, tick_size_minor_);
  const auto sink_after = checked_add(checked_add(fee_sink_, maker_fill.fee), taker_fill.fee);
  *maker = maker_after;
  *taker = taker_after;
  fee_sink_ = sink_after;
  last_trade_ = trade.id;
  return LedgerEntry{
      .trade_id = trade.id,
      .notional = value,
      .buyer = maker_buys ? trade.maker_agent_id : trade.taker_agent_id,
      .seller = maker_buys ? trade.taker_agent_id : trade.maker_agent_id,
      .buyer_fee = maker_buys ? maker_fill.fee : taker_fill.fee,
      .seller_fee = maker_buys ? taker_fill.fee : maker_fill.fee,
  };
}

LedgerTotals Ledger::totals() const {
  LedgerTotals totals;
  totals.fee_sink = fee_sink_;
  for (const auto& account : accounts_) {
    totals.initial_cash = checked_add(totals.initial_cash, account.initial_cash);
    totals.cash = checked_add(totals.cash, account.cash);
    totals.initial_position = checked_add(totals.initial_position, account.initial_position);
    totals.position = checked_add(totals.position, account.position);
  }
  return totals;
}

void Ledger::verify() const {
  const auto sums = totals();
  if (sums.position != sums.initial_position) {
    throw RunError{"AccountingImbalance", "shares are not conserved across accounts"};
  }
  if (checked_add(sums.cash, sums.fee_sink) != sums.initial_cash) {
    throw RunError{"AccountingImbalance", "cash plus the fee sink differs from opening cash"};
  }
  for (const auto& account : accounts_) {
    const auto books = checked_add(
        checked_subtract(checked_add(account.cash, account.cost_basis), account.realized_pnl),
        account.fees_paid);
    const auto shares = Int128{account.initial_position} +
                        static_cast<Int128>(account.bought.value()) -
                        static_cast<Int128>(account.sold.value());
    if (books != checked_add(account.initial_cash, account.initial_cost_basis) ||
        shares != Int128{account.position}) {
      throw RunError{"AccountingImbalance", "account " + std::to_string(account.agent.value()) +
                                                " does not reconcile with its fills"};
    }
  }
}

} // namespace markettwin
