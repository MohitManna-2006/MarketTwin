#ifndef MARKETTWIN_ACCOUNTING_ACCOUNTING_CONFIG_HPP
#define MARKETTWIN_ACCOUNTING_ACCOUNTING_CONFIG_HPP

#include "markettwin/accounting/fees.hpp"
#include "markettwin/common/ids.hpp"

#include <optional>
#include <vector>

namespace markettwin {

inline constexpr std::int64_t kMaxConfiguredCashMinor = 1'000'000'000'000'000'000;

// Per-agent exceptions to the accounting defaults.
struct AccountConfig {
  AgentId agent;
  Money initial_cash;
  // Unset: the agent's cash is unconstrained and may go negative. Set (>= 0): before a
  // scenario agent's buy intent becomes an order, the environment requires
  //   delivered cash + credit_limit >= cost of every working buy + cost of this one
  // and drops the intent otherwise. Fills are never rejected: the exchange has already
  // executed them. See ADR 0004 for what this does and does not guarantee.
  std::optional<Money> credit_limit;
  bool operator==(const AccountConfig&) const = default;
};

// The default (no fees, zero opening cash, no constraints) is what a schema 1 or 2 run
// config means, and such a config keeps its canonical bytes and digest.
struct AccountingConfig {
  FeeSchedule fees;
  Money default_initial_cash;
  std::vector<AccountConfig> accounts; // at most one per registered agent
  bool operator==(const AccountingConfig&) const = default;
};

struct RunConfig;

// Throws RunError{"InvalidConfig"} unless fees, balances, and accounts are in bounds.
void validate_accounting(const RunConfig& config);
[[nodiscard]] Money initial_cash(const AccountingConfig& accounting, AgentId agent) noexcept;
[[nodiscard]] std::optional<Money> credit_limit(const AccountingConfig& accounting,
                                                AgentId agent) noexcept;

} // namespace markettwin
#endif // MARKETTWIN_ACCOUNTING_ACCOUNTING_CONFIG_HPP
