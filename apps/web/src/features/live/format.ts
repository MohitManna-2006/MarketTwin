import { UNAVAILABLE } from "@/lib/formatting";

const MICRO = 1_000_000;

/**
 * Exact US-dollar text of an integer micro-USD price: integer arithmetic only, so the
 * digits shown are the digits the core normalized. At least two decimals, more only when
 * the price has them.
 */
export function formatMicroUsd(value: number | null | undefined, minimumDecimals = 2): string {
  if (value === null || value === undefined) return UNAVAILABLE;
  const negative = value < 0;
  const magnitude = Math.abs(value);
  const whole = Math.floor(magnitude / MICRO);
  let fraction = String(magnitude % MICRO).padStart(6, "0");
  while (fraction.length > minimumDecimals && fraction.endsWith("0")) {
    fraction = fraction.slice(0, -1);
  }
  const grouped = whole.toLocaleString("en-US");
  return `${negative ? "−" : ""}${grouped}${fraction ? `.${fraction}` : ""}`;
}

/** An age for a status line: 0.4 s, 12 s, 3 min 05 s, 2 h 14 min. */
export function formatAge(milliseconds: number | null | undefined): string {
  if (milliseconds === null || milliseconds === undefined || !Number.isFinite(milliseconds)) {
    return UNAVAILABLE;
  }
  const ms = Math.max(0, milliseconds);
  if (ms < 10_000) return `${(ms / 1000).toFixed(1)} s`;
  const seconds = Math.floor(ms / 1000);
  if (seconds < 60) return `${seconds} s`;
  if (seconds < 3600) {
    return `${Math.floor(seconds / 60)} min ${String(seconds % 60).padStart(2, "0")} s`;
  }
  return `${Math.floor(seconds / 3600)} h ${String(Math.floor((seconds % 3600) / 60)).padStart(2, "0")} min`;
}

export type DisplayZone = "utc" | "eastern" | "local";

export const ZONE_LABEL: Record<DisplayZone, string> = {
  utc: "UTC",
  eastern: "ET",
  local: "Local",
};

function zoneOf(zone: DisplayZone): string | undefined {
  if (zone === "utc") return "UTC";
  return zone === "eastern" ? "America/New_York" : undefined;
}

const clockFormats = new Map<string, Intl.DateTimeFormat>();

/** HH:MM:SS in the display zone, for axis ticks and status lines. */
export function formatClock(epochMs: number, zone: DisplayZone): string {
  let format = clockFormats.get(zone);
  if (!format) {
    format = new Intl.DateTimeFormat("en-GB", {
      hour: "2-digit",
      minute: "2-digit",
      second: "2-digit",
      hour12: false,
      timeZone: zoneOf(zone),
    });
    clockFormats.set(zone, format);
  }
  return format.format(epochMs);
}

/**
 * A source timestamp in the display zone, keeping exactly the fractional digits the
 * provider sent: the digits are taken from the UTC nanosecond text, never recomputed.
 */
export function formatEventTime(timestampNs: string, precisionDigits: number, zone: DisplayZone) {
  const padded = timestampNs.padStart(10, "0");
  const epochMs = Number(padded.slice(0, -6));
  const fraction = padded.slice(-9).slice(0, Math.max(0, Math.min(9, precisionDigits)));
  return `${formatClock(epochMs, zone)}${fraction ? `.${fraction}` : ""}`;
}

/** Milliseconds since the epoch of a UTC nanosecond string. Sub-millisecond digits drop. */
export function nsToMs(timestampNs: string): number {
  return Number(timestampNs.slice(0, -6) || "0");
}

export function formatIsoClock(iso: string | null | undefined, zone: DisplayZone): string {
  if (!iso) return UNAVAILABLE;
  const ms = Date.parse(iso);
  return Number.isNaN(ms) ? UNAVAILABLE : `${formatClock(ms, zone)} ${ZONE_LABEL[zone]}`;
}
