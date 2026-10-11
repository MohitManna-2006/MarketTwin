// Number formatting for recorded integers.
//
// The core reports integers only: ticks, shares, minor currency units, and fixed-point
// scales (milli, ppm, ppb). Everything here is exact integer and string arithmetic, so a
// half-tick midpoint or a statistic in thousandths is printed as recorded, never through
// a floating-point division. A missing value is not zero and is never printed as one.

export const UNAVAILABLE = "—";
const MINUS = "−";

export type Maybe = number | null | undefined;

export function isPresent(value: Maybe): value is number {
  return value !== null && value !== undefined;
}

function group(digits: string): string {
  return digits.replace(/\B(?=(\d{3})+(?!\d))/g, ",");
}

function sign(negative: boolean, signed: boolean, zero: boolean): string {
  if (negative) return MINUS;
  return signed && !zero ? "+" : "";
}

export interface IntegerOptions {
  /** Print "+" on positive values. Zero never carries a sign. */
  signed?: boolean;
}

/** 15004 -> "15,004"; -830 -> "−830". */
export function formatInteger(value: Maybe, options: IntegerOptions = {}): string {
  if (!isPresent(value)) return UNAVAILABLE;
  if (!Number.isSafeInteger(value)) return "INVALID";
  const magnitude = Math.abs(value);
  return sign(value < 0, options.signed ?? false, value === 0) + group(String(magnitude));
}

/**
 * A fixed-point integer with `scale` decimal digits, for example milli-units (scale 3):
 * 45609100 -> "45,609.100". `digits` keeps fewer decimals, rounding half away from zero
 * as the core does.
 */
export function formatScaled(
  value: Maybe,
  scale: number,
  options: IntegerOptions & { digits?: number } = {},
): string {
  if (!isPresent(value)) return UNAVAILABLE;
  if (!Number.isSafeInteger(value)) return "INVALID";
  const digits = Math.min(options.digits ?? scale, scale);
  let magnitude = BigInt(Math.abs(value));
  const dropped = scale - digits;
  if (dropped > 0) {
    const divisor = 10n ** BigInt(dropped);
    magnitude = (magnitude + divisor / 2n) / divisor;
  }
  const unit = 10n ** BigInt(digits);
  const whole = group((magnitude / unit).toString());
  const fraction = digits > 0 ? "." + (magnitude % unit).toString().padStart(digits, "0") : "";
  const zero = magnitude === 0n;
  return sign(value < 0 && !zero, options.signed ?? false, zero) + whole + fraction;
}

/** A price recorded as twice its ticks, so a half-tick midpoint is an integer: 30001 -> "15,000.5". */
export function formatTwiceTicks(value: Maybe, options: { fixed?: boolean } = {}): string {
  if (!isPresent(value)) return UNAVAILABLE;
  if (!Number.isSafeInteger(value)) return "INVALID";
  const magnitude = Math.abs(value);
  const whole = group(String(Math.floor(magnitude / 2)));
  const half = magnitude % 2 === 1;
  const fraction = half ? ".5" : options.fixed ? ".0" : "";
  return (value < 0 ? MINUS : "") + whole + fraction;
}

/** Milliticks as ticks with three decimals: 15000149 -> "15,000.149". */
export function formatMilliticks(value: Maybe, options: IntegerOptions = {}): string {
  return formatScaled(value, 3, options);
}

/** Parts per million as a percentage: 121794 -> "12.1794%". */
export function formatPpmAsPercent(value: Maybe, digits = 2): string {
  const text = formatScaled(value, 4, { digits });
  return text === UNAVAILABLE || text === "INVALID" ? text : `${text}%`;
}

/** Bytes with a binary unit, for file sizes only. */
export function formatBytes(value: Maybe): string {
  if (!isPresent(value)) return UNAVAILABLE;
  const units = ["B", "KiB", "MiB", "GiB"];
  let amount = value;
  let unit = 0;
  while (amount >= 1024 && unit < units.length - 1) {
    amount /= 1024;
    unit += 1;
  }
  return `${unit === 0 ? String(amount) : amount.toFixed(1)} ${units[unit] ?? "B"}`;
}

export type Tone = "positive" | "negative" | "neutral";

/** Direction of a signed financial quantity. Zero and missing values have none. */
export function toneOf(value: Maybe): Tone {
  if (!isPresent(value) || value === 0) return "neutral";
  return value > 0 ? "positive" : "negative";
}

export const UNIT_LABELS: Record<string, string> = {
  minor: "minor",
  shares: "shares",
  millishares: "shares",
  ticks: "ticks",
  milliticks: "ticks",
  trades: "trades",
  ppm: "%",
  ppb: "ppb",
  millibps: "bps",
};

/**
 * A statistic the core reports in thousandths of a metric's unit. Some metrics are
 * themselves fixed-point (millishares, milliticks, millibps, ppm); those are folded into
 * the base unit so a reader sees shares, ticks, basis points, or percent.
 */
export function formatMilliOf(unit: string, value: Maybe, options: IntegerOptions = {}): string {
  switch (unit) {
    case "millishares":
    case "milliticks":
    case "millibps":
      return formatScaled(value, 6, { ...options, digits: 3 });
    case "ppm":
      return formatScaled(value, 7, { ...options, digits: 3 });
    case "ppb":
      return formatScaled(value, 3, { ...options, digits: 0 });
    default:
      return formatScaled(value, 3, { ...options, digits: 1 });
  }
}

/** A per-run metric value in the metric's own unit, folded the same way. */
export function formatMetricValue(
  unit: string,
  value: Maybe,
  options: IntegerOptions = {},
): string {
  switch (unit) {
    case "millishares":
    case "milliticks":
    case "millibps":
      return formatScaled(value, 3, options);
    case "ppm":
      return formatScaled(value, 4, { ...options, digits: 2 });
    default:
      return formatInteger(value, options);
  }
}

/** How many of the metric's recorded units make one displayed unit (for plot axes). */
export function unitDivisor(unit: string): number {
  switch (unit) {
    case "millishares":
    case "milliticks":
    case "millibps":
      return 1_000;
    case "ppm":
      return 10_000;
    default:
      return 1;
  }
}
