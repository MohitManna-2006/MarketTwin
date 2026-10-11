import { useEffect, useId, useLayoutEffect, useMemo, useRef } from "react";
import uPlot from "uplot";

import { cx } from "@/lib/cx";
import { TIME_INCREMENTS, formatElapsedTick } from "@/lib/formatting";
import { clamp } from "@/lib/time/asof";
import { usePlayback } from "@/state/playback";

// One time-series pane over elapsed simulation time. It fills its nearest positioned
// ancestor, so the pane's size is decided by the layout and never by the canvas.
//
// The x axis is elapsed nanoseconds since the start of the run: there is no calendar and
// no session clock. Sampled series are drawn as steps (last observation carried forward),
// so the picture never suggests a value between two recorded samples. Nothing animates.

export type SeriesKind = "step" | "line" | "bars" | "points";

export interface SeriesSpec {
  key: string;
  label: string;
  /** A CSS custom property name such as "--chart-price". */
  color: string;
  /** Plot coordinates, one per x. A missing observation is null and leaves a gap. */
  values: readonly (number | null)[];
  kind?: SeriesKind;
  scale?: string;
  width?: number;
  dash?: number[];
  /** Fills between the series and zero, at low opacity. */
  fill?: boolean;
  show?: boolean;
}

export interface ScaleSpec {
  /** Formats an axis tick, in the scale's display unit. */
  format: (value: number) => string;
  side?: "left" | "right";
  /** Keeps zero in view and draws a reference line there. */
  zero?: boolean;
  /** Smallest span to show, so a flat series does not fill the pane with noise. */
  minSpan?: number;
  axisWidth?: number;
}

export interface ChartEvent {
  ns: number;
  label: string;
}

export interface ChartBand {
  from: number;
  to: number;
}

export interface ChartMarker {
  ns: number;
  y: number;
  /** Up for a buy aggressor, down for a sell aggressor: shape carries the side. */
  shape: "up" | "down";
  scale?: string;
}

export type XRange = readonly [number, number];

export interface TimeSeriesChartProps {
  /** Elapsed simulation time of every plotted point, ns, ascending. */
  x: readonly number[];
  series: readonly SeriesSpec[];
  scales: Record<string, ScaleSpec>;
  /** Visible time range; null shows the whole run. Shared by panes that zoom together. */
  xRange: XRange | null;
  onXRange: (range: XRange | null) => void;
  /**
   * Elapsed time of every recorded sample. The playback cursor is an index into it; the
   * chart reads the cursor itself, so only the chart follows playback, not its parent.
   */
  cursorTimes?: readonly number[];
  /** A click (not a drag) asks to move the time cursor here. */
  onSeek?: (ns: number) => void;
  /** Index of the plotted point under the pointer, or null when it leaves. */
  onHover?: (index: number | null) => void;
  syncKey?: string;
  events?: readonly ChartEvent[];
  bands?: readonly ChartBand[];
  markers?: readonly ChartMarker[];
  /** Hides the x axis labels on all but the bottom pane of a stack. */
  showXAxis?: boolean;
  /** Names each event beside its rule. One pane of a stack is enough. */
  eventLabels?: boolean;
  label: string;
  description: string;
  className?: string;
}

function cssVar(name: string): string {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim() || "#888888";
}

function withAlpha(hex: string, alpha: number): string {
  const value = hex.replace("#", "");
  const channel = (index: number) => parseInt(value.slice(index * 2, index * 2 + 2), 16);
  return `rgba(${channel(0)}, ${channel(1)}, ${channel(2)}, ${alpha})`;
}

const FONT = '11px "IBM Plex Mono", ui-monospace, monospace';
const AXIS_GAP = 6;

export function TimeSeriesChart({
  x,
  series,
  scales,
  xRange,
  onXRange,
  cursorTimes,
  onSeek,
  onHover,
  syncKey,
  events,
  bands,
  markers,
  showXAxis = true,
  eventLabels = false,
  label,
  description,
  className,
}: TimeSeriesChartProps) {
  const container = useRef<HTMLDivElement>(null);
  const plot = useRef<uPlot | null>(null);
  const cursorLine = useRef<HTMLDivElement | null>(null);
  const descriptionId = useId();
  const cursorNs = usePlayback((state) =>
    cursorTimes ? (cursorTimes[state.index] ?? null) : null,
  );

  // Everything a uPlot callback reads lives in a ref, so the plot is built once per
  // series structure and never rebuilt because a handler or an overlay changed.
  const live = useRef({
    x,
    xRange,
    onXRange,
    onSeek,
    onHover,
    events,
    bands,
    markers,
    cursorNs,
    scales,
    eventLabels,
  });
  useLayoutEffect(() => {
    live.current = {
      x,
      xRange,
      onXRange,
      onSeek,
      onHover,
      events,
      bands,
      markers,
      cursorNs,
      scales,
      eventLabels,
    };
  });

  const structure = useMemo(
    () =>
      JSON.stringify([
        series.map((item) => [
          item.key,
          item.kind,
          item.scale,
          item.color,
          item.width,
          item.dash,
          item.fill,
        ]),
        Object.entries(scales).map(([key, scale]) => [
          key,
          scale.side,
          scale.zero,
          scale.axisWidth,
        ]),
        showXAxis,
        syncKey,
      ]),
    [series, scales, showXAxis, syncKey],
  );

  const data = useMemo<uPlot.AlignedData>(
    () => [x as number[], ...series.map((item) => item.values as (number | null)[])],
    [x, series],
  );

  function fullRange(): XRange {
    const points = live.current.x;
    const first = points[0] ?? 0;
    const last = points[points.length - 1] ?? first + 1;
    return last > first ? [first, last] : [first, first + 1];
  }

  function placeCursor() {
    const chart = plot.current;
    const line = cursorLine.current;
    if (!chart || !line) return;
    const ns = live.current.cursorNs;
    const { min, max } = chart.scales.x ?? {};
    if (
      ns === null ||
      min === undefined ||
      max === undefined ||
      min === null ||
      max === null ||
      ns < min ||
      ns > max
    ) {
      line.style.display = "none";
      return;
    }
    line.style.display = "block";
    line.style.transform = `translateX(${Math.round(chart.valToPos(ns, "x"))}px)`;
  }

  // Build (or rebuild) the plot when the set of series changes.
  useEffect(() => {
    const element = container.current;
    if (!element) return;
    const axisColor = cssVar("--chart-axis");
    const gridColor = cssVar("--chart-grid");
    const borderColor = cssVar("--border-default");
    const accent = cssVar("--accent-primary");
    const warning = cssVar("--data-warning");
    const positive = cssVar("--data-positive");
    const negative = cssVar("--data-negative");
    const canvas = cssVar("--canvas");

    const scaleKeys = Object.keys(scales);
    const uScales: uPlot.Scales = {
      x: { time: false, range: (_u, min, max) => [min, max] },
    };
    for (const key of scaleKeys) {
      uScales[key] = {
        auto: true,
        range: (_u, min, max) => {
          const spec = live.current.scales[key];
          let low = min;
          let high = max;
          if (low === null || high === null || !Number.isFinite(low) || !Number.isFinite(high)) {
            return [0, 1];
          }
          if (spec?.zero) {
            low = Math.min(low, 0);
            high = Math.max(high, 0);
          }
          const minimum = spec?.minSpan ?? 0;
          if (high - low < minimum) {
            const middle = (high + low) / 2;
            low = middle - minimum / 2;
            high = middle + minimum / 2;
          }
          const padding = (high - low) * 0.08 || 1;
          return [low - padding, high + padding];
        },
      };
    }

    const axes: uPlot.Axis[] = [
      {
        scale: "x",
        show: showXAxis,
        stroke: axisColor,
        font: FONT,
        size: 24,
        gap: 4,
        incrs: TIME_INCREMENTS,
        space: 96,
        grid: { stroke: gridColor, width: 1 },
        ticks: { stroke: borderColor, width: 1, size: 4 },
        values: (_u, splits, _axis, _space, increment) =>
          splits.map((value) => formatElapsedTick(value, increment)),
      },
      ...scaleKeys.map<uPlot.Axis>((key, index) => {
        const spec = scales[key];
        return {
          scale: key,
          // uPlot's axis sides: 1 is the right edge, 3 the left.
          // eslint-disable-next-line @typescript-eslint/no-unsafe-enum-assignment
          side: spec?.side === "right" ? 1 : 3,
          stroke: axisColor,
          font: FONT,
          size: spec?.axisWidth ?? 68,
          gap: AXIS_GAP,
          space: 28,
          grid: { show: index === 0, stroke: gridColor, width: 1 },
          ticks: { stroke: borderColor, width: 1, size: 4 },
          values: (_u, splits) =>
            splits.map((value) => live.current.scales[key]?.format(value) ?? ""),
        };
      }),
    ];

    const uSeries: uPlot.Series[] = [
      {},
      ...series.map<uPlot.Series>((item) => {
        const color = cssVar(item.color);
        const kind = item.kind ?? "step";
        const paths =
          kind === "step"
            ? uPlot.paths.stepped?.({ align: 1 })
            : kind === "bars"
              ? uPlot.paths.bars?.({ size: [0.7, 8, 1], align: 1 })
              : kind === "points"
                ? () => null
                : uPlot.paths.linear?.();
        return {
          label: item.label,
          scale: item.scale ?? scaleKeys[0] ?? "y",
          stroke: color,
          width: kind === "bars" ? 0 : (item.width ?? 1.25),
          dash: item.dash,
          fill:
            kind === "bars"
              ? withAlpha(color, 0.55)
              : item.fill
                ? withAlpha(color, 0.1)
                : undefined,
          fillTo: item.fill || kind === "bars" ? 0 : undefined,
          paths,
          spanGaps: false,
          show: item.show ?? true,
          points:
            kind === "points"
              ? { show: true, size: 4, fill: color, stroke: color }
              : { show: false },
        };
      }),
    ];

    let panning: { startX: number; range: XRange } | null = null;
    let pressed: { x: number; y: number } | null = null;

    function setRange(range: XRange) {
      const [low, high] = fullRange();
      const span = clamp(range[1] - range[0], 1, high - low);
      const start = clamp(range[0], low, high - span);
      const next: XRange = [start, start + span];
      live.current.onXRange(next[0] <= low && next[1] >= high ? null : next);
    }

    const options: uPlot.Options = {
      width: Math.max(120, element.clientWidth),
      height: Math.max(60, element.clientHeight),
      pxAlign: 1,
      padding: [8, 30, showXAxis ? 0 : 4, 0],
      legend: { show: false },
      scales: uScales,
      axes,
      series: uSeries,
      cursor: {
        x: true,
        y: false,
        points: { show: false },
        drag: { x: true, y: false, setScale: true, dist: 5 },
        sync: syncKey ? { key: syncKey, setSeries: false } : undefined,
        bind: {
          mousedown: (_u, _target, handler) => (event: MouseEvent) => {
            pressed = { x: event.clientX, y: event.clientY };
            if (event.shiftKey) {
              const chart = plot.current;
              const scale = chart?.scales.x;
              if (scale && typeof scale.min === "number" && typeof scale.max === "number") {
                panning = { startX: event.clientX, range: [scale.min, scale.max] };
              }
              return null;
            }
            return handler(event);
          },
          dblclick: () => () => {
            live.current.onXRange(null);
            return null;
          },
        },
      },
      hooks: {
        setCursor: [
          (chart) => {
            const index = chart.cursor.idx;
            live.current.onHover?.(typeof index === "number" ? index : null);
          },
        ],
        setScale: [
          (chart, key) => {
            if (key !== "x") return;
            placeCursor();
            const { min, max } = chart.scales.x ?? {};
            if (typeof min !== "number" || typeof max !== "number") return;
            const wanted = live.current.xRange ?? fullRange();
            if (Math.abs(wanted[0] - min) > 0.5 || Math.abs(wanted[1] - max) > 0.5)
              setRange([min, max]);
          },
        ],
        setSize: [() => placeCursor()],
        drawClear: [
          (chart) => {
            const { ctx, bbox } = chart;
            const current = live.current;
            ctx.save();
            ctx.beginPath();
            ctx.rect(bbox.left, bbox.top, bbox.width, bbox.height);
            ctx.clip();
            // Scheduled shock intervals: a flat wash behind the data.
            for (const band of current.bands ?? []) {
              const left = chart.valToPos(band.from, "x", true);
              const right = chart.valToPos(band.to, "x", true);
              ctx.fillStyle = withAlpha(warning, 0.07);
              ctx.fillRect(left, bbox.top, right - left, bbox.height);
            }
            ctx.restore();
          },
        ],
        draw: [
          (chart) => {
            const { ctx, bbox } = chart;
            const current = live.current;
            const ratio = uPlot.pxRatio;
            ctx.save();
            ctx.beginPath();
            ctx.rect(bbox.left, bbox.top, bbox.width, bbox.height);
            ctx.clip();

            // A zero reference for scales that have a meaningful zero.
            for (const key of scaleKeys) {
              if (!current.scales[key]?.zero) continue;
              const y = Math.round(chart.valToPos(0, key, true)) + 0.5;
              ctx.strokeStyle = cssVar("--border-strong");
              ctx.lineWidth = ratio;
              ctx.setLineDash([]);
              ctx.beginPath();
              ctx.moveTo(bbox.left, y);
              ctx.lineTo(bbox.left + bbox.width, y);
              ctx.stroke();
            }

            // Trade markers: an upward triangle for a buy aggressor, downward for a sell.
            for (const marker of current.markers ?? []) {
              const px = chart.valToPos(marker.ns, "x", true);
              const py = chart.valToPos(marker.y, marker.scale ?? scaleKeys[0] ?? "y", true);
              const size = 3.5 * ratio;
              const up = marker.shape === "up";
              ctx.fillStyle = up ? positive : negative;
              ctx.strokeStyle = canvas;
              ctx.lineWidth = ratio;
              ctx.beginPath();
              ctx.moveTo(px, py + (up ? -size : size));
              ctx.lineTo(px - size, py + (up ? size : -size));
              ctx.lineTo(px + size, py + (up ? size : -size));
              ctx.closePath();
              ctx.fill();
              ctx.stroke();
            }

            // Scheduled shock transitions: a dashed rule at the scheduled time, labelled.
            ctx.font = `${10 * ratio}px "IBM Plex Sans", sans-serif`;
            ctx.textBaseline = "top";
            // uPlot leaves the alignment of its last axis label in place.
            ctx.textAlign = "left";
            let labelRow = 0;
            for (const event of current.events ?? []) {
              const px = Math.round(chart.valToPos(event.ns, "x", true)) + 0.5;
              ctx.strokeStyle = warning;
              ctx.lineWidth = ratio;
              ctx.setLineDash([4 * ratio, 3 * ratio]);
              ctx.beginPath();
              ctx.moveTo(px, bbox.top);
              ctx.lineTo(px, bbox.top + bbox.height);
              ctx.stroke();
              if (current.eventLabels) {
                const text = event.label.toUpperCase();
                const width = ctx.measureText(text).width;
                const flip = px + 6 * ratio + width > bbox.left + bbox.width;
                const tx = flip ? px - 6 * ratio - width : px + 6 * ratio;
                const ty = bbox.top + (4 + (labelRow % 3) * 14) * ratio;
                ctx.setLineDash([]);
                ctx.fillStyle = withAlpha(canvas, 0.85);
                ctx.fillRect(tx - 3 * ratio, ty - 2 * ratio, width + 6 * ratio, 13 * ratio);
                ctx.fillStyle = warning;
                ctx.fillText(text, tx, ty);
                labelRow += 1;
              }
            }
            ctx.restore();
          },
        ],
      },
    };

    const chart = new uPlot(options, data, element);
    plot.current = chart;

    const line = document.createElement("div");
    line.style.cssText = `position:absolute;top:0;bottom:0;left:0;width:1px;background:${accent};pointer-events:none;will-change:transform;z-index:2;`;
    chart.over.appendChild(line);
    cursorLine.current = line;

    const over = chart.over;
    function onMouseMove(event: MouseEvent) {
      if (!panning) return;
      const scale = (panning.range[1] - panning.range[0]) / Math.max(1, over.clientWidth);
      const shift = (panning.startX - event.clientX) * scale;
      setRange([panning.range[0] + shift, panning.range[1] + shift]);
    }
    function onMouseUp(event: MouseEvent) {
      const wasPanning = panning !== null;
      panning = null;
      if (!pressed) return;
      const moved = Math.hypot(event.clientX - pressed.x, event.clientY - pressed.y);
      pressed = null;
      if (wasPanning || moved > 3 || !over.contains(event.target as Node)) return;
      const bounds = over.getBoundingClientRect();
      live.current.onSeek?.(chart.posToVal(event.clientX - bounds.left, "x"));
    }
    function onWheel(event: WheelEvent) {
      const scale = chart.scales.x;
      if (!scale || typeof scale.min !== "number" || typeof scale.max !== "number") return;
      event.preventDefault();
      const bounds = over.getBoundingClientRect();
      const anchor = chart.posToVal(event.clientX - bounds.left, "x");
      const factor = event.deltaY < 0 ? 0.8 : 1.25;
      setRange([anchor - (anchor - scale.min) * factor, anchor + (scale.max - anchor) * factor]);
    }
    function onLeave() {
      live.current.onHover?.(null);
    }
    document.addEventListener("mousemove", onMouseMove);
    document.addEventListener("mouseup", onMouseUp);
    over.addEventListener("wheel", onWheel, { passive: false });
    over.addEventListener("mouseleave", onLeave);

    const initial = live.current.xRange ?? fullRange();
    chart.setScale("x", { min: initial[0], max: initial[1] });
    placeCursor();
    void document.fonts.ready.then(() => chart.redraw(false));

    const observer = new ResizeObserver(() => {
      const width = Math.max(120, element.clientWidth);
      const height = Math.max(60, element.clientHeight);
      if (width !== chart.width || height !== chart.height) chart.setSize({ width, height });
    });
    observer.observe(element);

    return () => {
      observer.disconnect();
      document.removeEventListener("mousemove", onMouseMove);
      document.removeEventListener("mouseup", onMouseUp);
      over.removeEventListener("wheel", onWheel);
      over.removeEventListener("mouseleave", onLeave);
      chart.destroy();
      plot.current = null;
      cursorLine.current = null;
    };
    // The plot is rebuilt only when the series structure changes; data, range, overlays,
    // and handlers reach it through the effects below and the `live` ref.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [structure]);

  // New data: keep the visible range, or show everything.
  useEffect(() => {
    const chart = plot.current;
    if (!chart) return;
    chart.setData(data, false);
    const range = live.current.xRange ?? fullRange();
    chart.setScale("x", { min: range[0], max: range[1] });
  }, [data]);

  // A range chosen in another pane, or a reset.
  useEffect(() => {
    const chart = plot.current;
    if (!chart) return;
    const range = xRange ?? fullRange();
    const { min, max } = chart.scales.x ?? {};
    if (min !== range[0] || max !== range[1]) chart.setScale("x", { min: range[0], max: range[1] });
  }, [xRange]);

  useEffect(() => {
    const chart = plot.current;
    if (!chart) return;
    series.forEach((item, index) => {
      const shown = item.show ?? true;
      if (chart.series[index + 1]?.show !== shown) chart.setSeries(index + 1, { show: shown });
    });
  }, [series]);

  // Overlays are drawn by hooks; ask for a redraw when they change.
  useEffect(() => {
    plot.current?.redraw(false);
  }, [events, bands, markers]);

  useEffect(placeCursor);

  return (
    <div
      ref={container}
      role="img"
      aria-label={label}
      aria-describedby={descriptionId}
      className={cx("absolute inset-0 overflow-hidden", className)}
    >
      <p id={descriptionId} className="sr-only">
        {description}
      </p>
    </div>
  );
}
