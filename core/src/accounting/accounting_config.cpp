#include "markettwin/accounting/accounting_config.hpp"

#include "markettwin/simulation/config.hpp"

#include <algorithm>
#include <set>

namespace markettwin {
namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw RunError{"InvalidConfig", message};
  }
}

bool cash_in_bounds(Money cash) noexcept {
  return cash.value() >= -kMaxConfiguredCashMinor && cash.value() <= kMaxConfiguredCashMinor;
}

const AccountConfig* find(const AccountingConfig& accounting, AgentId agent) noexcept {
  const auto found = std::ranges::find(accounting.accounts, agent, &AccountConfig::agent);
  return found == accounting.accounts.end() ? nullptr : &*found;
}

} // namespace

void validate_accounting(const RunConfig& config) {
  const auto& accounting = config.accounting;
  const auto rate_ok = [](std::int64_t rate) { return rate >= -kMaxFeePpm && rate <= kMaxFeePpm; };
  require(rate_ok(accounting.fees.maker_ppm) && rate_ok(accounting.fees.taker_ppm),
          "fee rates must be within -1e6 to 1e6 ppm");
  require(cash_in_bounds(accounting.default_initial_cash),
          "default initial cash must be within 1e18 minor units");
  std::set<AgentId> seen;
  for (const auto& account : accounting.accounts) {
    require(seen.insert(account.agent).second, "accounting accounts must name each agent once");
    require(std::ranges::find(config.exchange.agents, account.agent) !=
                config.exchange.agents.end(),
            "accounting account names an agent that is not registered with the exchange");
    require(cash_in_bounds(account.initial_cash), "initial cash must be within 1e18 minor units");
    if (account.credit_limit) {
      require(account.credit_limit->value() >= 0 && cash_in_bounds(*account.credit_limit),
              "credit limit must be 0 to 1e18 minor units");
      // The check lives where intents become orders; scripted orders have no such point.
      require(config.scenario &&
                  std::ranges::find(config.scenario->agents, account.agent, &AgentConfig::id) !=
                      config.scenario->agents.end(),
              "a credit limit may only be set for a scenario agent");
    }
  }
}

Money initial_cash(const AccountingConfig& accounting, AgentId agent) noexcept {
  const auto* account = find(accounting, agent);
  return account == nullptr ? accounting.default_initial_cash : account->initial_cash;
}

std::optional<Money> credit_limit(const AccountingConfig& accounting, AgentId agent) noexcept {
  const auto* account = find(accounting, agent);
  return account == nullptr ? std::nullopt : account->credit_limit;
}

} // namespace markettwin
