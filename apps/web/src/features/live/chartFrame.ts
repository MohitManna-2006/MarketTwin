import type uPlot from "uplot";

import type { SeriesBuffer } from "./liveModel";

// What the live chart draws, kept apart from the plotting library so it can be reasoned
// about, and tested, as data.

export const SERIES = [
  { key: "trade", label: "Trades", color: "--chart-price" },
  { key: "bid", label: "Bid", color: "--data-positive" },
  { key: "ask", label: "Ask", color: "--data-negative" },
  { key: "mid", label: "Midpoint", color: "--chart-baseline" },
] as const;
export type SeriesKey = (typeof SERIES)[number]["key"];

export interface ChartFrame {
  data: uPlot.AlignedData;
  range: [number, number];
}

/** The rows inside the window that ends at the newest observation. */
export function chartFrame(buffer: SeriesBuffer, windowMs: number): ChartFrame {
  const count = buffer.x.length;
  const end = count > 0 ? (buffer.x[count - 1] ?? 0) : 0;
  // With less history than the window, the picture starts at the first observation.
  const start = Math.max(end - windowMs, count > 0 ? (buffer.x[0] ?? end) : end);
  // Keep one row before the window so a step line enters from the left edge.
  let first = 0;
  let low = 0;
  let high = count;
  while (low < high) {
    const middle = (low + high) >> 1;
    if ((buffer.x[middle] ?? 0) < start) low = middle + 1;
    else high = middle;
  }
  first = Math.max(0, low - 1);
  return {
    data: [
      buffer.x.slice(first),
      buffer.trade.slice(first),
      buffer.bid.slice(first),
      buffer.ask.slice(first),
      buffer.mid.slice(first),
    ],
    range: [start, end > start ? end : start + 1],
  };
}
