import type { ChartBand, ChartEvent } from "@/components/charts/TimeSeriesChart";
import type { ShockView } from "@/lib/api/types";

const KIND: Record<string, string> = {
  FundamentalJump: "Fundamental jump",
  LiquidityWithdrawal: "Liquidity withdrawal",
  VolatilityRegime: "Volatility regime",
};

export function shockLabel(kind: string): string {
  return KIND[kind] ?? kind;
}

/**
 * Chart annotations for a run's shocks, from the transitions the trace recorded. These
 * mark when a shock was scheduled to act; what the market then did is in the data.
 */
export function shockAnnotations(shocks: readonly ShockView[]): {
  events: ChartEvent[];
  bands: ChartBand[];
} {
  const events: ChartEvent[] = [];
  const bands: ChartBand[] = [];
  for (const shock of shocks) {
    const name = shockLabel(shock.kind);
    for (const transition of shock.transitions) {
      const interval = shock.transitions.length > 1;
      events.push({
        ns: transition.timestamp_ns,
        label: interval ? `${name} ${transition.phase === "Begin" ? "begins" : "ends"}` : name,
      });
    }
    const begin = shock.transitions.find((item) => item.phase === "Begin");
    const end = shock.transitions.find((item) => item.phase === "End");
    if (begin && end) bands.push({ from: begin.timestamp_ns, to: end.timestamp_ns });
  }
  return { events, bands };
}
