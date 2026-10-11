import type { ReactNode } from "react";

import type { CursorSample } from "@/features/runs/cursor";
import type { MarketMetrics, RunDetail } from "@/lib/api/types";
import { cx } from "@/lib/cx";
import {
  UNAVAILABLE,
  formatElapsedAuto,
  formatInteger,
  formatMilliticks,
  formatPpmAsPercent,
  formatScaled,
  formatTwiceTicks,
  toneOf,
  type Tone,
} from "@/lib/formatting";

const TONE: Record<Tone, string> = {
  positive: "text-pos",
  negative: "text-neg",
  neutral: "text-fg",
};

function Line({
  label,
  value,
  unit,
  note,
  tone = "neutral",
  title,
}: {
  label: string;
  value: string;
  unit?: string;
  note?: ReactNode;
  tone?: Tone;
  title?: string;
}) {
  const missing = value === UNAVAILABLE;
  return (
    <div
      className="flex h-[22px] items-center justify-between gap-2 border-b border-line-subtle px-3"
      title={title}
    >
      <dt className="flex min-w-0 items-baseline gap-2">
        <span className="label shrink-0 text-fg-3">{label}</span>
        {note ? <span className="min-w-0 truncate text-2xs text-fg-3">{note}</span> : null}
      </dt>
      <dd className="flex shrink-0 items-baseline gap-1">
        <span className={cx("num text-sm", missing ? "text-fg-3" : TONE[tone])}>{value}</span>
        {unit && !missing ? <span className="text-2xs text-fg-3">{unit}</span> : null}
      </dd>
    </div>
  );
}

function Heading({ title, note }: { title: string; note: string }) {
  return (
    <h3 className="flex h-6 shrink-0 items-center gap-2 border-b border-line bg-surface-1 px-3">
      <span className="label text-fg-2">{title}</span>
      <span className="flex-1" />
      <span className="num text-2xs font-normal text-fg-3">{note}</span>
    </h3>
  );
}

function bookState(sample: CursorSample): string {
  const market = sample.market;
  if (!market) return UNAVAILABLE;
  const bid = market.bid_price_ticks !== null;
  const ask = market.ask_price_ticks !== null;
  if (bid && ask) return "Two-sided";
  if (bid) return "Bid only";
  if (ask) return "Ask only";
  return "Empty";
}

function share(part: number, total: number): string {
  if (total <= 0) return UNAVAILABLE;
  // Parts per million of the run, rounded to nearest; shown as a percentage.
  return formatPpmAsPercent(Math.round((part / total) * 1_000_000), 4);
}

/**
 * Market statistics in two groups that are never mixed: values recorded at the sample
 * under the cursor, and statistics the core computes only for the whole run.
 */
export function MarketTelemetry({ run, sample }: { run: RunDetail; sample: CursorSample }) {
  const market = sample.market;
  const totals = (run.metrics as { market: MarketMetrics } | null)?.market ?? null;
  const mid =
    market && sample.markSource === "Midpoint" && market.mark_twice_ticks !== null
      ? market.mark_twice_ticks
      : null;
  // Midpoint minus fundamental, in milliticks: both operands are recorded integers.
  const gap =
    mid !== null && market && market.fundamental_milliticks !== null
      ? mid * 500 - market.fundamental_milliticks
      : null;

  return (
    <div
      tabIndex={0}
      role="group"
      aria-label="Market telemetry values"
      className="h-full overflow-auto"
    >
      <Heading title="As of cursor" note={formatElapsedAuto(sample.timestampNs)} />
      <dl>
        <Line label="Book state" value={bookState(sample)} />
        <Line
          label="Best bid"
          value={formatInteger(market?.bid_price_ticks)}
          unit="ticks"
          note={
            market?.bid_quantity != null ? `${formatInteger(market.bid_quantity)} sh` : undefined
          }
          tone={market?.bid_price_ticks != null ? "positive" : "neutral"}
        />
        <Line
          label="Best ask"
          value={formatInteger(market?.ask_price_ticks)}
          unit="ticks"
          note={
            market?.ask_quantity != null ? `${formatInteger(market.ask_quantity)} sh` : undefined
          }
          tone={market?.ask_price_ticks != null ? "negative" : "neutral"}
        />
        <Line label="Spread" value={formatInteger(market?.spread_ticks)} unit="ticks" />
        <Line
          label="Mark"
          value={formatTwiceTicks(market?.mark_twice_ticks, { fixed: true })}
          unit="ticks"
          note={
            sample.markSource
              ? `source: ${sample.markSource.replace(/([a-z])([A-Z])/g, "$1 $2").toLowerCase()}`
              : undefined
          }
          title="The price accounts are valued at: midpoint, else last trade, else the scenario reference"
        />
        <Line
          label="Fundamental"
          value={formatMilliticks(market?.fundamental_milliticks)}
          unit="ticks"
          title="The simulated latent value. It is never a trade price and never a mark."
        />
        <Line
          label="Mid − fundamental"
          value={formatMilliticks(gap, { signed: true })}
          unit="ticks"
          tone={toneOf(gap)}
          title="Recorded midpoint minus recorded fundamental"
        />
        <Line label="Trades" value={formatInteger(market?.trade_count)} note="cumulative" />
        <Line
          label="Volume"
          value={formatInteger(market?.volume)}
          unit="shares"
          note="cumulative"
        />
        <Line
          label="Bid depth"
          value={formatInteger(market?.bid_depth)}
          unit="shares"
          note={market?.depth_levels != null ? `${market.depth_levels} levels` : undefined}
        />
        <Line
          label="Ask depth"
          value={formatInteger(market?.ask_depth)}
          unit="shares"
          note={market?.depth_levels != null ? `${market.depth_levels} levels` : undefined}
        />
      </dl>

      <Heading title="Run total" note="not available as of a time" />
      <dl>
        <Line label="VWAP" value={formatMilliticks(totals?.vwap_milliticks)} unit="ticks" />
        <Line
          label="Trade range"
          value={
            totals?.low_trade_price_ticks != null && totals.high_trade_price_ticks != null
              ? `${formatInteger(totals.low_trade_price_ticks)} – ${formatInteger(totals.high_trade_price_ticks)}`
              : UNAVAILABLE
          }
          unit="ticks"
        />
        <Line
          label="Mean spread"
          value={formatMilliticks(totals?.mean_spread_milliticks)}
          unit="ticks"
          note={totals ? `${formatInteger(totals.two_sided_samples)} two-sided samples` : undefined}
        />
        <Line
          label="Return volatility"
          value={formatScaled(totals?.midpoint_return_volatility_ppb, 5, { digits: 3 })}
          unit="bps / sample"
          title="Sample standard deviation of midpoint returns between consecutive two-sided samples. Not annualized."
        />
        <Line
          label="Two-sided time"
          value={totals ? share(totals.two_sided_ns, totals.total_ns) : UNAVAILABLE}
          title="Exact share of logical time the book had both a bid and an ask"
        />
        <Line
          label="Fill rate"
          value={formatPpmAsPercent(totals?.fill_rate_ppm)}
          note="shares filled / submitted"
        />
        <Line
          label="Orders rejected"
          value={formatInteger(totals?.orders_rejected)}
          note={
            totals && Object.keys(totals.rejections).length > 0
              ? Object.entries(totals.rejections)
                  .map(
                    ([reason, count]) =>
                      `${reason.replace(/([a-z])([A-Z])/g, "$1 $2").toLowerCase()} ${formatInteger(count)}`,
                  )
                  .join(", ")
              : undefined
          }
        />
      </dl>
    </div>
  );
}
