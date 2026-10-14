import { useEffect, useRef } from "react";
import uPlot from "uplot";

import { SERIES, chartFrame, type SeriesKey } from "./chartFrame";
import { formatClock, type DisplayZone } from "./format";
import { liveModel } from "./liveStore";

// The selected instrument's observed prices over a sliding window.
//
// The x axis is the source event time of each observation; its right edge is the newest
// observation, not the browser's clock, so a quiet feed shows a still picture instead of a
// line creeping forward through time in which nothing was observed. Quotes are drawn as
// steps and trades as marks: nothing is interpolated between two observations.
//
// Drawing is decoupled from the stream. The plot is built once and refilled on an
// animation frame only when the buffer has changed, so cost follows the display.

function cssVar(name: string): string {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim() || "#888888";
}

const FONT = '11px "IBM Plex Mono", ui-monospace, monospace';

export interface LiveChartProps {
  symbol: string;
  windowMs: number;
  /** Freezes the picture. Observations keep arriving and are drawn on resume. */
  paused: boolean;
  zone: DisplayZone;
  visible: Record<SeriesKey, boolean>;
}

export function LiveChart({ symbol, windowMs, paused, zone, visible }: LiveChartProps) {
  const container = useRef<HTMLDivElement>(null);
  const plot = useRef<uPlot | null>(null);
  const view = useRef({ symbol, windowMs, paused, zone, visible });
  const drawn = useRef({ symbol: "", version: -1, windowMs: 0 });
  useEffect(() => {
    view.current = { symbol, windowMs, paused, zone, visible };
    const chart = plot.current;
    if (chart) {
      SERIES.forEach((item, index) => {
        if (chart.series[index + 1]?.show !== visible[item.key]) {
          chart.setSeries(index + 1, { show: visible[item.key] });
        }
      });
      // A new zone relabels the same instants.
      chart.redraw(false, true);
    }
  });

  useEffect(() => {
    const element = container.current;
    if (!element) return;
    const axisColor = cssVar("--chart-axis");
    const gridColor = cssVar("--chart-grid");
    const borderColor = cssVar("--border-default");
    const stepped = uPlot.paths.stepped?.({ align: 1 });
    const chart = new uPlot(
      {
        width: Math.max(160, element.clientWidth),
        height: Math.max(120, element.clientHeight),
        padding: [10, 8, 0, 0],
        legend: { show: false },
        cursor: { drag: { x: false, y: false }, points: { size: 5 } },
        scales: {
          x: { time: false, range: (_u, min, max) => [min, max] },
          y: {
            auto: true,
            range: (_u, min, max) => {
              if (min === null || max === null || !Number.isFinite(min) || !Number.isFinite(max)) {
                return [0, 1];
              }
              const padding = (max - min) * 0.12 || Math.max(0.01, Math.abs(max) * 0.0005);
              return [min - padding, max + padding];
            },
          },
        },
        axes: [
          {
            scale: "x",
            stroke: axisColor,
            font: FONT,
            size: 24,
            gap: 4,
            space: 96,
            grid: { stroke: gridColor, width: 1 },
            ticks: { stroke: borderColor, width: 1, size: 4 },
            values: (_u, splits) => splits.map((value) => formatClock(value, view.current.zone)),
          },
          {
            scale: "y",
            // uPlot's axis sides: 1 is the right edge, where a price axis belongs.
            // eslint-disable-next-line @typescript-eslint/no-unsafe-enum-assignment
            side: 1,
            stroke: axisColor,
            font: FONT,
            size: 68,
            gap: 6,
            space: 28,
            grid: { stroke: gridColor, width: 1 },
            ticks: { stroke: borderColor, width: 1, size: 4 },
            values: (_u, splits) => {
              const step = splits.length > 1 ? Math.abs((splits[1] ?? 0) - (splits[0] ?? 0)) : 1;
              const decimals = step >= 0.01 ? 2 : step >= 0.001 ? 3 : 4;
              return splits.map((value) => value.toFixed(decimals));
            },
          },
        ],
        series: [
          {},
          {
            label: "Trades",
            scale: "y",
            stroke: cssVar("--chart-price"),
            width: 1,
            paths: stepped,
            spanGaps: true,
            points: { show: true, size: 4, width: 1, fill: cssVar("--chart-price") },
          },
          {
            label: "Bid",
            scale: "y",
            stroke: cssVar("--data-positive"),
            width: 1.25,
            paths: stepped,
            spanGaps: false,
            points: { show: false },
          },
          {
            label: "Ask",
            scale: "y",
            stroke: cssVar("--data-negative"),
            width: 1.25,
            paths: stepped,
            spanGaps: false,
            points: { show: false },
          },
          {
            label: "Midpoint",
            scale: "y",
            stroke: cssVar("--chart-baseline"),
            width: 1,
            dash: [3, 3],
            paths: stepped,
            spanGaps: false,
            points: { show: false },
          },
        ],
      },
      [[], [], [], [], []],
      element,
    );
    plot.current = chart;
    drawn.current = { symbol: "", version: -1, windowMs: 0 };

    const observer = new ResizeObserver(() => {
      chart.setSize({
        width: Math.max(160, element.clientWidth),
        height: Math.max(120, element.clientHeight),
      });
    });
    observer.observe(element);

    let frame = 0;
    const draw = () => {
      frame = requestAnimationFrame(draw);
      const current = view.current;
      const buffer = liveModel().seriesOf(current.symbol);
      const stale =
        drawn.current.symbol !== current.symbol ||
        drawn.current.windowMs !== current.windowMs ||
        drawn.current.version !== buffer.version;
      // Changing instrument or window is the reader's own action and redraws even while
      // paused; new observations do not.
      const own =
        drawn.current.symbol !== current.symbol || drawn.current.windowMs !== current.windowMs;
      if (!stale || (current.paused && !own)) return;
      drawn.current = {
        symbol: current.symbol,
        version: buffer.version,
        windowMs: current.windowMs,
      };
      const next = chartFrame(buffer, current.windowMs);
      chart.batch(() => {
        chart.setData(next.data, false);
        chart.setScale("x", { min: next.range[0], max: next.range[1] });
      });
    };
    frame = requestAnimationFrame(draw);
    return () => {
      cancelAnimationFrame(frame);
      observer.disconnect();
      chart.destroy();
      plot.current = null;
    };
  }, []);

  return (
    <div className="absolute inset-0" data-testid="live-chart">
      <div
        ref={container}
        role="img"
        aria-label={`${symbol} observed trades, bid, ask, and midpoint over source event time`}
        className="size-full"
      />
    </div>
  );
}
