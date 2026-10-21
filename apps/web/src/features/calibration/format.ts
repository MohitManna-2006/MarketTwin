// Display of calibration integers. The core stores each fact as an integer in a stated
// unit; these functions only move the decimal point and never round a value the reader
// could mistake for an exact one.

/** Fact names in ADR 0011 order, grouped as the score weighs them. */
export const FACT_ORDER = [
  "spread_cents_p10",
  "spread_cents_p50",
  "spread_cents_p90",
  "relative_spread_bps_p50",
  "trades_per_minute",
  "interarrival_cv",
  "empty_minute_share",
  "trade_return_lag1_autocorr",
  "midpoint_vol_1m_bps",
  "two_sided_share",
  "l1_updates_per_minute",
  "l1_median_duration_ms",
] as const;

export const FACT_LABELS: Record<string, string> = {
  spread_cents_p10: "Spread p10",
  spread_cents_p50: "Spread p50",
  spread_cents_p90: "Spread p90",
  relative_spread_bps_p50: "Relative spread p50",
  trades_per_minute: "Trades per minute",
  interarrival_cv: "Inter-arrival CV",
  empty_minute_share: "Minutes without a trade",
  trade_return_lag1_autocorr: "Lag-1 autocorrelation",
  midpoint_vol_1m_bps: "1-minute midpoint volatility",
  two_sided_share: "Two-sided time",
  l1_updates_per_minute: "L1 updates per minute",
  l1_median_duration_ms: "Median L1 duration",
};

export const PARAMETER_LABELS: Record<string, string> = {
  noise_interval_ns: "Noise mean wakeup interval",
  noise_market_order_ppm: "Noise market-order share",
  noise_max_quantity: "Noise max quantity",
  noise_limit_offset_ticks: "Noise limit offset",
  maker_interval_ns: "Maker wakeup interval",
  maker_half_spread_ticks: "Maker half-spread",
  maker_quote_size: "Maker quote size",
  maker_level_spacing_ticks: "Maker level spacing",
  fundamental_volatility_milliticks: "Fundamental step volatility",
};

function fixed(value: number, scale: number, digits: number): string {
  return (value / scale).toFixed(digits);
}

/** A fact value with its unit, e.g. "1.000 ¢", "42.0 %", "-0.338". */
export function formatFact(name: string, value: number | null | undefined): string {
  if (value === null || value === undefined) return "undefined";
  switch (name) {
    case "spread_cents_p10":
    case "spread_cents_p50":
    case "spread_cents_p90":
      return `${fixed(value, 1000, 3)} ¢`;
    case "relative_spread_bps_p50":
    case "midpoint_vol_1m_bps":
      return `${fixed(value, 1000, 3)} bp`;
    case "trades_per_minute":
    case "l1_updates_per_minute":
      return `${fixed(value, 1000, 3)} /min`;
    case "l1_median_duration_ms":
      return `${fixed(value, 1000, 3)} ms`;
    case "empty_minute_share":
    case "two_sided_share":
      return `${fixed(value, 10_000, 2)} %`;
    case "interarrival_cv":
    case "trade_return_lag1_autocorr":
      return fixed(value, 1_000_000, 4);
    default:
      return String(value);
  }
}

/** A distance or score in millionths of a scale, as scales. */
export function formatScales(ppm: number | null | undefined): string {
  if (ppm === null || ppm === undefined) return "—";
  return fixed(ppm, 1_000_000, 2);
}

export function formatParameter(name: string, value: number | null | undefined): string {
  if (value === null || value === undefined) return "baseline";
  if (name.endsWith("_interval_ns")) return `${fixed(value, 1_000_000, 0)} ms`;
  if (name.endsWith("_ppm")) return `${fixed(value, 10_000, 1)} %`;
  if (name.endsWith("_ticks")) return `${value} ¢`;
  if (name === "fundamental_volatility_milliticks") return `${fixed(value, 1000, 3)} ¢`;
  return String(value);
}
