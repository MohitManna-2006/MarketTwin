import type { StatSummary } from "@/lib/api/types";

const LABELS: Record<string, string> = {
  total_pnl: "Total P&L",
  realized_pnl: "Realized P&L",
  unrealized_pnl: "Unrealized P&L",
  fees_paid: "Fees paid",
  max_drawdown: "Max drawdown",
  maker_spread_capture: "Maker spread capture",
  final_abs_position: "Final |position|",
  max_abs_position: "Max |position|",
  mean_abs_position: "Mean |position|",
  traded_quantity: "Traded quantity",
  fill_rate: "Fill rate",
  trade_count: "Trade count",
  volume: "Volume",
  mean_spread: "Mean spread",
  mean_relative_spread: "Mean relative spread",
  mean_bid_depth: "Mean bid depth",
  mean_ask_depth: "Mean ask depth",
  midpoint_return_volatility: "Midpoint return volatility",
  two_sided_time: "Two-sided time",
};

export function metricLabel(name: string): string {
  return LABELS[name] ?? name.replaceAll("_", " ");
}

/** The metrics the brief names for a maker comparison, offered as shortcuts. */
export const HEADLINE_METRICS = [
  "total_pnl",
  "max_drawdown",
  "mean_abs_position",
  "final_abs_position",
  "maker_spread_capture",
];

/**
 * Where a 95% Student t interval of a mean paired difference sits relative to zero. This
 * restates the interval; it is not a verdict on a strategy, and it carries no colour.
 */
export function againstZero(difference: StatSummary | null): string {
  const interval = difference?.mean_t_interval_milli;
  if (!interval) return difference ? "no interval: one pair" : "no pairs";
  if (interval[0] === 0 && interval[1] === 0) return "identical";
  if (interval[0] > 0) return "above zero";
  if (interval[1] < 0) return "below zero";
  return "includes zero";
}
