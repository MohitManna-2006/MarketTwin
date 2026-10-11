import type { AgentConfig, PolicyConfig } from "@/lib/api/types";
import {
  formatDuration,
  formatInteger,
  formatMilliticks,
  formatPpmAsPercent,
} from "@/lib/formatting";

export const POLICY_LABEL: Record<string, string> = {
  MarketMaker: "Market maker",
  InventoryMarketMaker: "Inventory-aware maker",
  Momentum: "Momentum",
  Value: "Value",
  Noise: "Noise",
};

export const POLICY_SHORT: Record<string, string> = {
  MarketMaker: "Maker",
  InventoryMarketMaker: "Inv. maker",
  Momentum: "Momentum",
  Value: "Value",
  Noise: "Noise",
};

function n(policy: PolicyConfig, key: string): number {
  return Number(policy[key]);
}

/** One line that says what a policy is configured to do, in its own units. */
export function policySummary(policy: PolicyConfig): string {
  switch (policy.kind) {
    case "MarketMaker":
      return `±${n(policy, "half_spread_ticks")} ticks · ${formatInteger(n(policy, "quote_size"))} sh × ${n(policy, "levels")} levels`;
    case "InventoryMarketMaker":
      return `±${n(policy, "half_spread_ticks")} ticks · ${formatInteger(n(policy, "quote_size"))} sh × ${n(policy, "levels")} levels · skew ≤ ${n(policy, "max_skew_ticks")} ticks`;
    case "Momentum":
      return `windows ${n(policy, "short_window")} / ${n(policy, "long_window")} · ${formatInteger(n(policy, "order_size"))} sh · signal ≥ ${formatMilliticks(n(policy, "threshold_milliticks"))} ticks`;
    case "Value":
      return `edge ≥ ${formatMilliticks(n(policy, "threshold_milliticks"))} ticks · ${formatInteger(n(policy, "order_size"))} sh per threshold`;
    case "Noise":
      return `${n(policy, "min_quantity")}–${n(policy, "max_quantity")} sh · ${formatPpmAsPercent(n(policy, "market_order_ppm"), 0)} market orders`;
    default:
      return policy.kind;
  }
}

export function wakeupSummary(agent: AgentConfig): string {
  const { interval_ns, distribution } = agent.wakeup;
  return `${formatDuration(interval_ns)} ${distribution === "Fixed" ? "fixed" : "mean, exp."}`;
}
