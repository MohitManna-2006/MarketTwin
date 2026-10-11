#ifndef MARKETTWIN_ACCOUNTING_REPORT_HPP
#define MARKETTWIN_ACCOUNTING_REPORT_HPP

#include "markettwin/accounting/valuation.hpp"
#include "markettwin/telemetry/metrics.hpp"

#include <array>
#include <string>

namespace markettwin {

// Versions of the accounting stream (accounting.jsonl) and of the metric definitions
// (metrics.json). Either changes when a record layout, formula, or rounding rule does.
inline constexpr std::uint64_t kAccountingSchema = 1;
inline constexpr std::uint64_t kMetricsSchema = 1;

// One agent at the end of a run. Money is in minor units, quantities in shares.
struct AgentReport {
  std::string policy; // empty for an agent driven only by scripted commands
  Account account;
  // At the final mark. Empty only for an open position with no mark at all.
  std::optional<Valuation> valuation;
  // Equity is observed at the start of the run, at every sample, and at the end, each
  // time at the mark of that moment. Peak and drawdown are over those observations.
  std::uint64_t equity_observations = 0;
  std::optional<Money> peak_equity;
  std::optional<Money> max_drawdown;
  // Mean of |position| over the sampling schedule. Empty when the run took no sample.
  std::optional<std::int64_t> mean_abs_position_millishares;
  std::optional<Money> credit_limit;
  std::uint64_t cash_refusals = 0; // buy intents dropped by the cash constraint
  AgentFlow flow;
  Quantity resting_quantity;                  // still in the book at the end
  std::optional<std::uint64_t> fill_rate_ppm; // (maker + taker quantity) / submitted quantity
  // Edge against the pre-trade midpoint (see AgentFlow), converted to money.
  Money maker_spread_capture;
  Money taker_spread_cost;
  bool operator==(const AgentReport&) const = default;
};

// Everything Phase 2B computes for one run, in memory. metrics.json is this, serialized.
struct RunReport {
  Mark final_mark;
  MarketMetrics market;
  Money traded_notional; // sum over trades, each counted once
  LedgerTotals totals;
  std::array<std::uint64_t, 4> samples_by_mark{}; // indexed by MarkSource
  std::vector<AgentReport> agents;                // by agent ID
  bool operator==(const RunReport&) const = default;
};

} // namespace markettwin
#endif // MARKETTWIN_ACCOUNTING_REPORT_HPP
