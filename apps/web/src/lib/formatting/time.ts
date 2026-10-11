// Simulation time is elapsed nanoseconds since the start of a run. A scenario defines no
// market-session clock, so no time of day is ever shown: only elapsed time.

import { UNAVAILABLE, type Maybe, isPresent } from "./numbers";

const NS_PER_MS = 1_000_000;
const NS_PER_S = 1_000_000_000;

function pad(value: number, width = 2): string {
  return String(value).padStart(width, "0");
}

export type ElapsedPrecision = "s" | "ms" | "ns";

/**
 * Elapsed simulation time as HH:MM:SS, with milliseconds or the full nanoseconds:
 * 1_002_304_274 -> "00:00:01", "00:00:01.002", "00:00:01.002 304 274".
 */
export function formatElapsed(ns: Maybe, precision: ElapsedPrecision = "ms"): string {
  if (!isPresent(ns)) return UNAVAILABLE;
  const whole = Math.floor(ns / NS_PER_S);
  const rest = ns - whole * NS_PER_S;
  const clock = `${pad(Math.floor(whole / 3600))}:${pad(Math.floor(whole / 60) % 60)}:${pad(whole % 60)}`;
  if (precision === "s") return clock;
  const digits = pad(rest, 9);
  if (precision === "ms") return `${clock}.${digits.slice(0, 3)}`;
  return `${clock}.${digits.slice(0, 3)} ${digits.slice(3, 6)} ${digits.slice(6)}`;
}

/** Shows nanoseconds only where a timestamp has them. */
export function formatElapsedAuto(ns: Maybe): string {
  if (!isPresent(ns)) return UNAVAILABLE;
  return formatElapsed(ns, ns % NS_PER_MS === 0 ? "ms" : "ns");
}

const DURATION_UNITS: readonly (readonly [number, string])[] = [
  [3600 * NS_PER_S, "h"],
  [60 * NS_PER_S, "min"],
  [NS_PER_S, "s"],
  [NS_PER_MS, "ms"],
  [1_000, "µs"],
  [1, "ns"],
];

/** A configured duration in its largest exact unit: 500_000_000 -> "500 ms". */
export function formatDuration(ns: Maybe): string {
  if (!isPresent(ns)) return UNAVAILABLE;
  if (ns === 0) return "0 ns";
  for (const [size, unit] of DURATION_UNITS) {
    if (ns >= size && ns % size === 0) return `${(ns / size).toLocaleString("en-US")} ${unit}`;
  }
  return `${ns.toLocaleString("en-US")} ns`;
}

/** A duration with up to two units, for horizons: 5_400 s -> "1 h 30 min". */
export function formatSpan(ns: Maybe): string {
  if (!isPresent(ns)) return UNAVAILABLE;
  const seconds = Math.floor(ns / NS_PER_S);
  if (seconds < 1) return formatDuration(ns);
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.floor(seconds / 60) % 60;
  const rest = seconds % 60;
  const parts = [
    hours > 0 ? `${hours} h` : "",
    minutes > 0 ? `${minutes} min` : "",
    rest > 0 ? `${rest} s` : "",
  ].filter(Boolean);
  return parts.slice(0, 2).join(" ");
}

/** Axis tick increments, in ns, that land on round elapsed times. */
export const TIME_INCREMENTS: number[] = [
  1, 2, 5, 10, 20, 50, 100, 200, 500, 1e3, 2e3, 5e3, 1e4, 2e4, 5e4, 1e5, 2e5, 5e5, 1e6, 2e6, 5e6,
  1e7, 2e7, 5e7, 1e8, 2e8, 5e8, 1e9, 2e9, 5e9, 10e9, 15e9, 30e9, 60e9, 120e9, 300e9, 600e9, 900e9,
  1800e9, 3600e9, 7200e9, 14400e9, 21600e9, 43200e9, 86400e9,
];

/** A tick label no more precise than the spacing between ticks. */
export function formatElapsedTick(ns: number, increment: number): string {
  if (increment >= NS_PER_S) return formatElapsed(ns, "s");
  if (increment >= NS_PER_MS) return formatElapsed(ns, "ms");
  return formatElapsed(ns, "ns");
}

/** Wall-clock creation time (UTC, from the registry), never simulation time. */
export function formatWallClock(iso: string | null | undefined): string {
  if (!iso) return UNAVAILABLE;
  const date = new Date(iso);
  if (Number.isNaN(date.getTime())) return "INVALID";
  return `${date.getUTCFullYear()}-${pad(date.getUTCMonth() + 1)}-${pad(date.getUTCDate())} ${pad(
    date.getUTCHours(),
  )}:${pad(date.getUTCMinutes())}:${pad(date.getUTCSeconds())}Z`;
}

/** Wall-clock processing time: 502 -> "502 ms", 12_400 -> "12.4 s". */
export function formatProcessing(ms: Maybe): string {
  if (!isPresent(ms)) return UNAVAILABLE;
  if (ms < 1000) return `${ms} ms`;
  if (ms < 60_000) return `${(ms / 1000).toFixed(1)} s`;
  return `${Math.floor(ms / 60_000)} min ${Math.round((ms % 60_000) / 1000)} s`;
}

export const SECOND_NS = NS_PER_S;
