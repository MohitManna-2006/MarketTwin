import { useMutation, useQuery } from "@tanstack/react-query";
import { useEffect, useMemo, useState } from "react";
import { useParams } from "react-router";

import { ContextBar } from "@/components/shell/ContextBar";
import { Page } from "@/components/shell/TerminalShell";
import { PlaybackControls } from "@/components/timeline/PlaybackControls";
import { Button } from "@/components/ui/Button";
import { InstrumentPanel } from "@/components/ui/InstrumentPanel";
import { ErrorState, LoadingState } from "@/components/ui/States";
import { usePlaybackClock } from "@/hooks/usePlaybackClock";
import { post } from "@/lib/api/client";
import {
  historicalEventsQuery,
  historicalSeriesQuery,
  historicalSessionQuery,
} from "@/lib/api/queries";
import type { HistoricalCapture, HistoricalSample } from "@/lib/api/types";
import { usePlayback } from "@/state/playback";

function price(value: number | null | undefined) {
  return value === null || value === undefined ? "—" : `$${(value / 1_000_000).toFixed(6)}`;
}

function MiniChart({ rows, index }: { rows: HistoricalSample[]; index: number }) {
  const values = rows.flatMap((row) =>
    [row.last_trade_micro_usd, row.bid_micro_usd, row.ask_micro_usd].filter(
      (value): value is number => value !== null,
    ),
  );
  if (values.length === 0)
    return <div className="p-4 text-sm text-fg-3">No price observations.</div>;
  const low = Math.min(...values);
  const high = Math.max(...values);
  const width = 900;
  const height = 260;
  const x = (at: number) => (rows.length < 2 ? 0 : (at / (rows.length - 1)) * width);
  const y = (value: number) => height - ((value - low) / Math.max(1, high - low)) * height;
  const path = (key: "last_trade_micro_usd" | "bid_micro_usd" | "ask_micro_usd") => {
    let result = "";
    let previous: number | null = null;
    rows.forEach((row, at) => {
      const value = row[key];
      if (value === null) {
        previous = null;
      } else if (previous === null) {
        result += `M${x(at)},${y(value)} `;
        previous = value;
      } else {
        result += `H${x(at)} V${y(value)} `;
        previous = value;
      }
    });
    return result;
  };
  return (
    <svg
      role="img"
      aria-label="Recorded historical trade, bid, and ask prices"
      viewBox={`0 0 ${width} ${height}`}
      preserveAspectRatio="none"
      className="h-full min-h-48 w-full"
    >
      <path d={path("bid_micro_usd")} fill="none" stroke="var(--data-positive)" strokeWidth="1.5" />
      <path d={path("ask_micro_usd")} fill="none" stroke="var(--data-negative)" strokeWidth="1.5" />
      <path
        d={path("last_trade_micro_usd")}
        fill="none"
        stroke="var(--chart-price)"
        strokeWidth="2"
      />
      <line
        x1={x(index)}
        x2={x(index)}
        y1={0}
        y2={height}
        stroke="var(--chart-axis)"
        strokeWidth="1"
      />
    </svg>
  );
}

export function HistoricalSessionPage() {
  const { sessionId = "" } = useParams();
  const session = useQuery(historicalSessionQuery(sessionId));
  const series = useQuery(historicalSeriesQuery(sessionId));
  const events = useQuery(historicalEventsQuery(sessionId));
  const [zone, setZone] = useState<"utc" | "eastern">("utc");
  const verify = useMutation({
    mutationFn: () =>
      post<{ verified: boolean; code?: string }>(`/historical/sessions/${sessionId}/verify`),
  });
  const rows = series.data?.records ?? [];
  const index = usePlayback((state) => state.index);
  usePlaybackClock();
  useEffect(
    () => usePlayback.getState().attach(`historical:${sessionId}`, rows.length, 0),
    [rows.length, sessionId],
  );
  const current = rows[index] ?? null;
  const currentEvent = events.data?.records.find((event) => event.sequence === current?.sequence);
  const timestamp = useMemo(() => {
    if (!currentEvent) return "—";
    if (zone === "utc") return currentEvent.source_timestamp;
    return new Date(currentEvent.source_timestamp).toLocaleString("en-US", {
      timeZone: "America/New_York",
      timeZoneName: "short",
    });
  }, [currentEvent, zone]);

  if (session.isPending || series.isPending || events.isPending)
    return <LoadingState label="Loading historical session" />;
  if (session.isError || series.isError || events.isError) {
    return (
      <ErrorState
        error={session.error ?? series.error ?? events.error}
        onRetry={() => {
          void session.refetch();
          void series.refetch();
          void events.refetch();
        }}
      />
    );
  }
  const detail = session.data;
  const capture = (detail.manifest?.metadata as { capture?: HistoricalCapture } | undefined)
    ?.capture;
  return (
    <Page
      context={
        <ContextBar
          crumbs={[
            { label: "Historical", to: "/historical" },
            { label: detail.symbol },
            { label: detail.id, verbatim: true },
          ]}
          status={
            <span className="label border border-warn px-2 py-0.5 text-warn">
              {detail.observation_kind}
            </span>
          }
        >
          <Button size="sm" onClick={() => verify.mutate()} disabled={verify.isPending}>
            {verify.isPending ? "Verifying…" : "Verify replay"}
          </Button>
        </ContextBar>
      }
    >
      <div className="grid h-full min-h-[680px] grid-cols-1 grid-rows-[minmax(300px,1fr)_auto_auto] gap-px bg-line-subtle lg:grid-cols-[2fr_1fr] lg:grid-rows-[minmax(320px,1fr)_280px]">
        <InstrumentPanel
          index="01"
          title="Observed price"
          subtitle="trade / bid / ask · discrete records, no interpolation"
          actions={
            <div className="flex items-center gap-2">
              <button
                type="button"
                className="label text-fg-3 hover:text-fg"
                onClick={() => setZone(zone === "utc" ? "eastern" : "utc")}
              >
                {zone === "utc" ? "UTC" : "ET"}
              </button>
              <PlaybackControls />
            </div>
          }
        >
          <MiniChart rows={rows} index={index} />
        </InstrumentPanel>
        <InstrumentPanel index="02" title="Observation state" subtitle="as of playback cursor">
          <dl className="grid grid-cols-2 gap-x-4 gap-y-2 p-4 text-sm">
            {[
              ["Timestamp", timestamp],
              ["Sequence", current?.sequence ?? "—"],
              ["Last trade", price(current?.last_trade_micro_usd)],
              ["Best bid", price(current?.bid_micro_usd)],
              ["Best ask", price(current?.ask_micro_usd)],
              [
                "Quoted spread (derived)",
                current?.bid_micro_usd !== null &&
                current?.bid_micro_usd !== undefined &&
                current?.ask_micro_usd !== null &&
                current?.ask_micro_usd !== undefined
                  ? price(current.ask_micro_usd - current.bid_micro_usd)
                  : "—",
              ],
              ["Bid size", current?.bid_size ?? "—"],
              ["Ask size", current?.ask_size ?? "—"],
              ["Current trade size", currentEvent?.quantity ?? "—"],
              ["Quote state", current?.quote_state ?? "—"],
            ].map(([label, value]) => (
              <div key={String(label)}>
                <dt className="label text-fg-3">{label}</dt>
                <dd className="num mt-0.5 break-words text-fg">{value}</dd>
              </div>
            ))}
          </dl>
          <p className="border-t border-line p-3 text-xs text-fg-3">
            Quotes are recorded observations, not MarketTwin resting orders. Trade prints are not
            agent executions and do not establish accessible liquidity.
          </p>
        </InstrumentPanel>
        <InstrumentPanel
          index="03"
          title="Observed event stream"
          subtitle={`bounded window · ${events.data.records.length} records`}
          bodyClassName="overflow-auto"
        >
          <table className="w-full text-left text-xs">
            <thead className="sticky top-0 bg-surface-1 label text-fg-3">
              <tr>
                <th className="p-2">Seq</th>
                <th className="p-2">Type</th>
                <th className="p-2">Source time</th>
                <th className="p-2">Observation</th>
              </tr>
            </thead>
            <tbody>
              {events.data.records.map((event) => (
                <tr
                  key={event.sequence}
                  className={
                    event.sequence === current?.sequence
                      ? "bg-accent-wash text-fg"
                      : "border-t border-line-subtle text-fg-2"
                  }
                  onClick={() => usePlayback.getState().seek(event.sequence - 1)}
                >
                  <td className="num p-2">{event.sequence}</td>
                  <td className="label p-2">{event.kind}</td>
                  <td className="num p-2">{event.source_timestamp}</td>
                  <td className="num p-2">
                    {event.kind === "trade"
                      ? `${price(event.price_micro_usd)} × ${event.quantity ?? "—"}`
                      : `${price(event.bid_micro_usd)} / ${price(event.ask_micro_usd)}`}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </InstrumentPanel>
        <InstrumentPanel index="04" title="Quality and provenance" bodyClassName="overflow-auto">
          <div className="grid grid-cols-2 gap-3 p-4 text-sm">
            {[
              ["Provider", detail.provider],
              ["Feed / scope", `${detail.feed} / ${detail.metrics.scope}`],
              ["Records", detail.quality.records_imported],
              [
                "Trades / quotes",
                `${detail.quality.trade_records} / ${detail.quality.quote_records}`,
              ],
              ["Traded quantity", detail.quality.traded_quantity],
              ["Duplicates", detail.quality.duplicates],
              ["Out of order", detail.quality.out_of_order],
              [
                "Locked / crossed",
                `${detail.quality.locked_quotes} / ${detail.quality.crossed_quotes}`,
              ],
              ["Gaps > 5 min", detail.quality.gaps_over_five_minutes],
              ["VWAP", price(detail.metrics.vwap_micro_usd)],
              ["Mean spread", price(detail.metrics.mean_quoted_spread_micro_usd)],
              [
                "Trade return volatility",
                detail.metrics.trade_to_trade_return_volatility_ppb === null
                  ? "—"
                  : `${detail.metrics.trade_to_trade_return_volatility_ppb} ppb`,
              ],
            ].map(([label, value]) => (
              <div key={String(label)}>
                <div className="label text-fg-3">{label}</div>
                <div className="num mt-0.5 text-fg">{value}</div>
              </div>
            ))}
          </div>
          {(detail.quality.gap_markers ?? []).length > 0 ? (
            <div className="border-t border-line p-3">
              <p className="label mb-2 text-fg-3">Data gaps</p>
              <div className="flex flex-col gap-1">
                {(detail.quality.gap_markers ?? []).map((gap) => {
                  const target = rows.findIndex(
                    (row) => BigInt(row.timestamp_ns) >= BigInt(gap.to_timestamp_ns),
                  );
                  return (
                    <Button
                      key={`${gap.kind}-${gap.to_timestamp_ns}`}
                      size="sm"
                      className="justify-start"
                      disabled={target < 0}
                      onClick={() => usePlayback.getState().seek(target)}
                    >
                      Jump to {gap.kind} gap ending at {gap.to_timestamp_ns} ns
                    </Button>
                  );
                })}
              </div>
            </div>
          ) : null}
          {capture ? (
            <div className="border-t border-line p-3" data-testid="capture-provenance">
              <p className="label mb-2 text-fg-3">Live capture</p>
              <div className="grid grid-cols-2 gap-3 text-sm">
                {[
                  ["Recording", capture.recording_id],
                  ["Source", capture.source_label],
                  ["Captured", `${capture.started_at} → ${capture.stopped_at}`],
                  ["Stopped", capture.stop_reason.replaceAll("_", " ")],
                  ["Rejected records", capture.rejected_records],
                  [
                    "Corrections / cancels (not applied)",
                    Object.values(capture.unsupported_messages).reduce((sum, n) => sum + n, 0),
                  ],
                ].map(([label, value]) => (
                  <div key={String(label)}>
                    <div className="label text-fg-3">{label}</div>
                    <div className="num mt-0.5 break-words whitespace-normal text-fg">{value}</div>
                  </div>
                ))}
              </div>
              <p className="label mt-3 mb-1 text-fg-3">Coverage interruptions</p>
              {capture.coverage_gaps.length === 0 ? (
                <p className="text-xs text-fg-2">
                  No upstream interruption was recorded during this capture.
                </p>
              ) : (
                <ul className="flex flex-col gap-1">
                  {capture.coverage_gaps.map((gap) => (
                    <li
                      key={`${gap.from}-${gap.closed_by}`}
                      className="border-l-2 border-warn bg-surface-1 px-2 py-1 text-xs text-fg-2"
                    >
                      <span className="num">
                        {gap.from} → {gap.to ?? "unknown"}
                      </span>{" "}
                      ·{" "}
                      {gap.duration_ms === null
                        ? "duration unknown"
                        : `${(gap.duration_ms / 1000).toFixed(1)} s`}{" "}
                      · {gap.reason}
                    </li>
                  ))}
                </ul>
              )}
              <p className="mt-2 text-xs text-fg-3">{capture.disclaimer}</p>
            </div>
          ) : null}
          {verify.data ? (
            <p
              className={`border-t border-line p-3 text-sm ${verify.data.verified ? "text-pos" : "text-neg"}`}
            >
              {verify.data.verified
                ? "Native deterministic replay verified."
                : `Verification failed: ${verify.data.code ?? "unknown"}`}
            </p>
          ) : null}
          {verify.isError ? <ErrorState error={verify.error} compact /> : null}
        </InstrumentPanel>
      </div>
    </Page>
  );
}
