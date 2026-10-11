import { useQuery } from "@tanstack/react-query";
import { useMemo, useState } from "react";

import {
  TimeSeriesChart,
  type ChartMarker,
  type ScaleSpec,
  type SeriesSpec,
  type XRange,
} from "@/components/charts/TimeSeriesChart";
import { Button } from "@/components/ui/Button";
import { InstrumentPanel } from "@/components/ui/InstrumentPanel";
import { SegmentedControl, ToggleChip } from "@/components/ui/SegmentedControl";
import { EmptyState, ErrorState, LoadingState } from "@/components/ui/States";
import { shockAnnotations } from "@/features/runs/shocks";
import { useDebounced } from "@/hooks/useDebounced";
import { agentSeriesQuery, marketSeriesQuery, tradesQuery } from "@/lib/api/queries";
import type { MarketSeries, RunDetail } from "@/lib/api/types";
import {
  axisCompact,
  axisNumber,
  formatElapsed,
  formatInteger,
  formatMilliticks,
  formatTwiceTicks,
} from "@/lib/formatting";
import { indexAtOrBefore } from "@/lib/time/asof";
import { useChartHover } from "@/state/chartHover";
import { usePlayback } from "@/state/playback";

const MARKER_LIMIT = 1000;
type Secondary = "spread" | "volume" | "depth";

const PRICE_SCALE: Record<string, ScaleSpec> = {
  price: { format: (value) => axisNumber(value, Number.isInteger(value) ? 0 : 1), minSpan: 4 },
};
const SECONDARY_SCALES: Record<Secondary, Record<string, ScaleSpec>> = {
  spread: {
    y: {
      format: (value) => axisNumber(value, Number.isInteger(value) ? 0 : 1),
      zero: true,
      minSpan: 2,
    },
  },
  volume: { y: { format: axisCompact, zero: true, minSpan: 2 } },
  depth: { y: { format: axisCompact, zero: true, minSpan: 2 } },
};
const POSITION_SCALE: Record<string, ScaleSpec> = {
  y: { format: axisCompact, zero: true, minSpan: 4 },
};

function scaled(column: readonly (number | null)[], divisor: number): (number | null)[] {
  return column.map((value) => (value === null ? null : value / divisor));
}

interface Readout {
  label: string;
  color: string;
  value: string;
  unit: string;
}

function Legend({ series, sampleIndex }: { series: MarketSeries; sampleIndex: number }) {
  const hover = useChartHover((state) => state.index);
  const columns = series.columns;
  // Under the pointer when there is one; otherwise at the time cursor.
  const point = hover ?? indexAtOrBefore(series.index, sampleIndex);
  const at = columns.timestamp_ns[point] ?? null;
  const items: Readout[] = [
    {
      label: "Mid",
      color: "var(--chart-price)",
      value: formatTwiceTicks(columns.mid_twice_ticks[point], { fixed: true }),
      unit: "ticks",
    },
    {
      label: "Fundamental",
      color: "var(--chart-fundamental)",
      value: formatMilliticks(columns.fundamental_milliticks[point]),
      unit: "ticks",
    },
    {
      label: "Bid",
      color: "var(--data-positive)",
      value: formatInteger(columns.bid_price_ticks[point]),
      unit: "",
    },
    {
      label: "Ask",
      color: "var(--data-negative)",
      value: formatInteger(columns.ask_price_ticks[point]),
      unit: "",
    },
  ];
  return (
    <div className="flex h-6 shrink-0 items-center gap-4 border-b border-line-subtle px-3">
      <span className="label w-[178px] shrink-0 text-fg-3">
        {hover !== null ? "At pointer" : "At cursor"}{" "}
        <span className="num text-xs tracking-normal text-fg-2">{formatElapsed(at, "ms")}</span>
      </span>
      {items.map((item) => (
        <span key={item.label} className="flex items-baseline gap-1.5">
          <span
            aria-hidden
            className="inline-block h-0.5 w-2.5 self-center"
            style={{ background: item.color }}
          />
          <span className="label text-fg-3">{item.label}</span>
          <span className="num text-sm text-fg">{item.value}</span>
          {item.unit ? <span className="text-2xs text-fg-3">{item.unit}</span> : null}
        </span>
      ))}
    </div>
  );
}

export interface PricePanelProps {
  run: RunDetail;
  times: readonly number[];
  selectedAgent: number | null;
  xRange: XRange | null;
  onXRange: (range: XRange | null) => void;
}

/**
 * Midpoint against the simulated fundamental, with scheduled shocks and trades, over
 * elapsed simulation time. Sampled series are steps between recorded observations.
 */
export function PricePanel({ run, times, selectedAgent, xRange, onXRange }: PricePanelProps) {
  const market = useQuery(marketSeriesQuery(run.id));
  const agent = useQuery({
    ...agentSeriesQuery(run.id, selectedAgent ?? 0),
    enabled: selectedAgent !== null,
  });
  const [show, setShow] = useState({ mid: true, fundamental: true, quotes: false, trades: true });
  const [secondary, setSecondary] = useState<Secondary>("spread");
  const setHover = useChartHover((state) => state.set);
  const sampleIndex = usePlayback((state) => state.index);
  const { events, bands } = useMemo(() => shockAnnotations(run.shocks), [run.shocks]);

  const columns = market.data?.columns;
  const x = columns?.timestamp_ns as number[] | undefined;
  const first = x?.[0] ?? 0;
  const last = x?.[x.length - 1] ?? 0;

  const priceSeries = useMemo<SeriesSpec[]>(() => {
    if (!columns) return [];
    return [
      {
        key: "bid",
        label: "Best bid",
        color: "--data-positive",
        values: columns.bid_price_ticks,
        width: 1,
        show: show.quotes,
      },
      {
        key: "ask",
        label: "Best ask",
        color: "--data-negative",
        values: columns.ask_price_ticks,
        width: 1,
        show: show.quotes,
      },
      {
        key: "fundamental",
        label: "Fundamental",
        color: "--chart-fundamental",
        values: scaled(columns.fundamental_milliticks, 1000),
        width: 1.25,
        show: show.fundamental,
      },
      {
        key: "mid",
        label: "Midpoint",
        color: "--chart-price",
        values: scaled(columns.mid_twice_ticks, 2),
        width: 1.5,
        show: show.mid,
      },
    ].map((item) => ({ ...item, scale: "price" }));
  }, [columns, show.quotes, show.fundamental, show.mid]);

  const secondarySeries = useMemo<SeriesSpec[]>(() => {
    if (!columns) return [];
    if (secondary === "spread") {
      return [
        {
          key: "spread",
          label: "Spread",
          color: "--chart-baseline",
          values: columns.spread_ticks,
          width: 1,
        },
      ];
    }
    if (secondary === "volume") {
      // Shares traded between consecutive plotted samples: a difference of the recorded
      // cumulative volume, not an estimate.
      const volume = columns.volume;
      const delta = volume.map((value, index) => {
        const previous = index === 0 ? 0 : volume[index - 1];
        return value === null || previous === null || previous === undefined
          ? null
          : value - previous;
      });
      return [
        { key: "volume", label: "Volume", color: "--chart-baseline", values: delta, kind: "bars" },
      ];
    }
    return [
      {
        key: "bid-depth",
        label: "Bid depth",
        color: "--data-positive",
        values: columns.bid_depth,
        width: 1,
      },
      {
        key: "ask-depth",
        label: "Ask depth",
        color: "--data-negative",
        values: columns.ask_depth,
        width: 1,
      },
    ];
  }, [columns, secondary]);

  const positionSeries = useMemo<SeriesSpec[]>(() => {
    const data = agent.data;
    if (!data || selectedAgent === null) return [];
    return [
      {
        key: `position-${selectedAgent}`,
        label: "Position",
        color: "--chart-inventory-aware",
        values: data.columns.position,
        fill: true,
      },
    ];
  }, [agent.data, selectedAgent]);

  // Trade markers are requested for the visible range and drawn only when few enough to
  // read. Beyond that the count is stated and nothing is drawn.
  const range = useDebounced<XRange>(xRange ?? [first, last], 200);
  const trades = useQuery({
    ...tradesQuery(run.id, {
      fromNs: Math.ceil(range[0]),
      toNs: Math.floor(range[1]),
      agentId: selectedAgent ?? undefined,
      limit: MARKER_LIMIT,
    }),
    enabled: show.trades && x !== undefined && run.availability.trades,
    placeholderData: (previous) => previous,
  });
  const tradeTotal = trades.data?.total ?? null;
  const markers = useMemo<ChartMarker[]>(() => {
    if (!show.trades || !trades.data || trades.data.total > MARKER_LIMIT) return [];
    return trades.data.rows.map((trade) => ({
      ns: trade.timestamp_ns,
      y: trade.price_ticks,
      shape: trade.aggressor_side === "Buy" ? "up" : "down",
      scale: "price",
    }));
  }, [show.trades, trades.data]);

  function seek(ns: number) {
    const index = indexAtOrBefore(times, ns);
    usePlayback.getState().pause();
    usePlayback.getState().seek(Math.max(0, index));
  }

  const actions = (
    <>
      <ToggleChip
        label="Mid"
        swatch="var(--chart-price)"
        pressed={show.mid}
        onChange={(mid) => setShow({ ...show, mid })}
      />
      <ToggleChip
        label="Fundamental"
        swatch="var(--chart-fundamental)"
        pressed={show.fundamental}
        onChange={(fundamental) => setShow({ ...show, fundamental })}
        disabled={!run.availability.fundamental}
        title={run.availability.fundamental ? undefined : "A scripted run has no fundamental value"}
      />
      <ToggleChip
        label="Bid / ask"
        swatch="var(--text-secondary)"
        pressed={show.quotes}
        onChange={(quotes) => setShow({ ...show, quotes })}
      />
      <ToggleChip
        label="Trades"
        pressed={show.trades}
        onChange={(value) => setShow({ ...show, trades: value })}
        title="Mark trades at their price: an upward triangle for a buy aggressor, downward for a sell"
      />
      <Button size="sm" variant="ghost" disabled={xRange === null} onClick={() => onXRange(null)}>
        Reset zoom
      </Button>
    </>
  );

  let tradeNote = "";
  if (!show.trades) tradeNote = "trade markers off";
  else if (tradeTotal === null) tradeNote = "";
  else if (tradeTotal > MARKER_LIMIT) {
    tradeNote = `${formatInteger(tradeTotal)} trades in view · zoom to ${formatInteger(MARKER_LIMIT)} or fewer to mark them`;
  } else {
    tradeNote = `${formatInteger(tradeTotal)} trades marked · ▲ buy aggressor ▼ sell aggressor`;
  }

  return (
    <InstrumentPanel
      index="01"
      title="Market price"
      subtitle={
        market.data?.decimated
          ? `${formatInteger(market.data.index.length)} of ${formatInteger(market.data.sample_count)} samples plotted · each bucket keeps its extremes`
          : "midpoint and fundamental · steps between recorded samples"
      }
      unit="ticks"
      actions={actions}
      className="flex-1"
      bodyClassName="flex flex-col"
    >
      {market.isPending ? (
        <LoadingState label="Loading recorded samples" />
      ) : market.isError ? (
        <ErrorState error={market.error} onRetry={() => void market.refetch()} />
      ) : !x || x.length === 0 ? (
        <EmptyState title="No samples recorded">
          This run has no sampling schedule and took no scripted snapshots, so there is no price
          series to plot. The execution stream below still lists every recorded event.
        </EmptyState>
      ) : (
        <>
          <Legend series={market.data} sampleIndex={sampleIndex} />
          <div className="relative min-h-0 flex-[3]">
            <TimeSeriesChart
              x={x}
              series={priceSeries}
              scales={PRICE_SCALE}
              xRange={xRange}
              onXRange={onXRange}
              cursorTimes={times}
              onSeek={seek}
              onHover={setHover}
              syncKey={`run-${run.id}`}
              events={events}
              bands={bands}
              markers={markers}
              showXAxis={false}
              eventLabels
              label="Midpoint and simulated fundamental value over elapsed simulation time"
              description={`Stepped lines of the recorded midpoint and fundamental value across ${formatInteger(x.length)} plotted samples, from ${formatElapsed(first, "s")} to ${formatElapsed(last, "s")} elapsed. Exact values at the time cursor are in the telemetry panel.`}
            />
          </div>
          <div className="flex h-5 shrink-0 items-center gap-2 border-y border-line-subtle bg-surface-1 px-3">
            <SegmentedControl
              label="Secondary series"
              value={secondary}
              onChange={setSecondary}
              options={[
                { value: "spread", label: "Spread" },
                {
                  value: "volume",
                  label: "Volume",
                  title: "Shares traded between consecutive plotted samples",
                },
                {
                  value: "depth",
                  label: "Depth",
                  title: "Resting shares within the sampled levels of each side",
                },
              ]}
            />
            <span className="label text-fg-3">
              {secondary === "spread"
                ? "ticks"
                : secondary === "volume"
                  ? "shares per sample interval"
                  : "shares"}
            </span>
            <span className="flex-1" />
            <span className="truncate text-2xs text-fg-3">{tradeNote}</span>
          </div>
          <div className="relative min-h-0 flex-1">
            <TimeSeriesChart
              x={x}
              series={secondarySeries}
              scales={SECONDARY_SCALES[secondary]}
              xRange={xRange}
              onXRange={onXRange}
              cursorTimes={times}
              onSeek={seek}
              onHover={setHover}
              syncKey={`run-${run.id}`}
              events={events}
              bands={bands}
              showXAxis={selectedAgent === null}
              label={`${secondary} over elapsed simulation time`}
              description={`Recorded ${secondary} at each plotted sample.`}
            />
          </div>
          {selectedAgent !== null ? (
            <>
              <div className="flex h-5 shrink-0 items-center gap-2 border-y border-line-subtle bg-surface-1 px-3">
                <span className="label text-fg-2">
                  <span className="num font-medium tracking-normal text-accent-label">03</span>
                  <span className="text-fg-3"> / </span>
                  Agent {selectedAgent} inventory
                </span>
                <span className="label text-fg-3">shares, signed</span>
              </div>
              <div className="relative min-h-0 flex-1">
                {agent.isError ? (
                  <ErrorState error={agent.error} compact />
                ) : agent.data ? (
                  <TimeSeriesChart
                    x={agent.data.columns.timestamp_ns as number[]}
                    series={positionSeries}
                    scales={POSITION_SCALE}
                    xRange={xRange}
                    onXRange={onXRange}
                    cursorTimes={times}
                    onSeek={seek}
                    syncKey={`run-${run.id}`}
                    events={events}
                    bands={bands}
                    label={`Agent ${selectedAgent} position over elapsed simulation time`}
                    description="Recorded signed position at each plotted sample."
                  />
                ) : (
                  <LoadingState label="Loading agent series" />
                )}
              </div>
            </>
          ) : null}
        </>
      )}
    </InstrumentPanel>
  );
}
