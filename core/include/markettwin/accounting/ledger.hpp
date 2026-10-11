#ifndef MARKETTWIN_ACCOUNTING_LEDGER_HPP
#define MARKETTWIN_ACCOUNTING_LEDGER_HPP

#include "markettwin/accounting/fees.hpp"
#include "markettwin/exchange/execution.hpp"

#include <optional>
#include <vector>

namespace markettwin {

// Starting balances for one account. cost_basis is the signed entry value of the opening
// position (positive when long, negative when short, zero when flat).
struct AccountOpening {
  AgentId agent;
  Money cash;
  std::int64_t position = 0;
  Money cost_basis;
  bool operator==(const AccountOpening&) const = default;
};

// One agent's books. All money is in minor units; positions are signed shares.
//
// Accounting basis: average cost, carried as the total signed cost of the open position
// rather than as a per-share average, so no division happens until shares are released.
//   opening or adding      cost_basis += signed_shares * price
//   reducing by c of |q|   released = cost_basis * c / |q| (toward zero; all of it when c = |q|)
//                          realized_pnl += -(signed_shares * price) - released
//                          cost_basis -= released
//   reversing              the closing part as above, then the remainder opens at the price
// The identity cash + cost_basis - realized_pnl + fees_paid = opening cash + opening basis
// holds exactly after every fill.
struct Account {
  AgentId agent;
  Money initial_cash;
  std::int64_t initial_position = 0;
  Money initial_cost_basis;
  Money cash;
  std::int64_t position = 0;
  Money cost_basis;
  Money realized_pnl;      // gross of fees
  Money fees_paid;         // net of rebates; negative when rebates exceed fees
  Money gross_notional;    // sum of fill notionals, buys and sells alike
  Quantity bought;         // shares
  Quantity sold;           // shares
  std::uint64_t fills = 0; // one per trade side
  std::uint64_t max_abs_position = 0;
  Money min_cash; // lowest cash balance after any fill, or the opening balance
  bool operator==(const Account&) const = default;
};

// What one trade did, for the audit record.
struct LedgerEntry {
  TradeId trade_id;
  Money notional;
  AgentId buyer;
  AgentId seller;
  Money buyer_fee;
  Money seller_fee;
  bool operator==(const LedgerEntry&) const = default;
};

struct LedgerTotals {
  Money initial_cash;
  Money cash;
  Money fee_sink;
  std::int64_t initial_position = 0;
  std::int64_t position = 0;
  bool operator==(const LedgerTotals&) const = default;
};

// Double-entry books driven by authoritative exchange executions. A trade is booked into
// both parties or not at all, and never twice.
class Ledger {
public:
  // Throws RunError{"InvalidConfig"} for duplicate agents, a zero agent ID, a zero tick
  // size, or an opening basis whose sign disagrees with its position.
  Ledger(std::vector<AccountOpening> openings, const FeeSchedule& fees,
         std::uint64_t tick_size_minor);

  // Books one execution. Trade IDs must arrive in exchange order, starting at one:
  //   - the next ID is booked and its entry returned;
  //   - an ID already booked is ignored and nullopt is returned (duplicate report);
  //   - a later ID throws RunError{"TradeSequenceGap"}, because a trade went missing.
  // A malformed trade, an unknown party, or arithmetic overflow throws and leaves every
  // account exactly as it was.
  std::optional<LedgerEntry> apply(const Trade& trade);

  [[nodiscard]] const Account& account(AgentId agent) const;
  [[nodiscard]] const std::vector<Account>& accounts() const noexcept { return accounts_; } // by ID
  [[nodiscard]] Money fee_sink() const noexcept { return fee_sink_; }
  [[nodiscard]] TradeId last_trade() const noexcept { return last_trade_; }
  [[nodiscard]] std::uint64_t tick_size_minor() const noexcept { return tick_size_minor_; }
  [[nodiscard]] const FeeSchedule& fees() const noexcept { return fees_; }
  [[nodiscard]] LedgerTotals totals() const;
  // Throws RunError{"AccountingImbalance"} unless shares sum to the opening total, cash
  // plus the fee sink sums to the opening cash, and every account satisfies its identity.
  void verify() const;

private:
  [[nodiscard]] Account* find(AgentId agent) noexcept;

  std::vector<Account> accounts_; // sorted by agent ID
  FeeSchedule fees_;
  std::uint64_t tick_size_minor_;
  Money fee_sink_;
  TradeId last_trade_;
};

} // namespace markettwin
#endif // MARKETTWIN_ACCOUNTING_LEDGER_HPP
