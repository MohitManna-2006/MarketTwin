import { useMutation } from "@tanstack/react-query";
import { useState } from "react";

import { ContextBar } from "@/components/shell/ContextBar";
import { Page } from "@/components/shell/TerminalShell";
import { Button } from "@/components/ui/Button";
import { InstrumentPanel } from "@/components/ui/InstrumentPanel";
import { MetricReadout } from "@/components/ui/MetricReadout";
import { SegmentedControl, ToggleChip } from "@/components/ui/SegmentedControl";
import { ErrorState } from "@/components/ui/States";
import { StatusIndicator } from "@/components/ui/StatusIndicator";
import { post } from "@/lib/api/client";
import { cx } from "@/lib/cx";
import { UNAVAILABLE } from "@/lib/formatting";

import { FeedDiagnostics } from "./FeedDiagnostics";
import { LiveChart } from "./LiveChart";
import { SERIES, type SeriesKey } from "./chartFrame";
import { LiveTape } from "./LiveTape";
import { MarketStatusPanel } from "./MarketStatusPanel";
import { Watchlist } from "./Watchlist";
import { ACTIVITY_STATUS, FEED_STATUS, agesOf, headline, noticesOf } from "./derive";
import { ZONE_LABEL, formatAge, formatEventTime, formatMicroUsd, type DisplayZone } from "./format";
import { useLiveConnection, useLiveModel, useNow } from "./hooks";
import { useLive } from "./liveStore";
import type { FeedName } from "./protocol";

const WINDOWS = [
  { value: "60000", label: "1 m" },
  { value: "300000", label: "5 m" },
  { value: "900000", label: "15 m" },
  { value: "1800000", label: "30 m" },
];

const HEADLINE_TONE: Record<string, string> = {
  positive: "border-pos-mark text-pos",
  negative: "border-neg-mark text-neg",
  warning: "border-warn text-warn",
  neutral: "border-info text-info",
  active: "border-accent text-accent",
  muted: "border-line-strong text-fg-2",
};

const FEED_OPTIONS: { value: FeedName; label: string; title: string }[] = [
  { value: "iex", label: "IEX", title: "Alpaca IEX: one venue's trades and quotes" },
  { value: "test", label: "Test", title: "Alpaca test stream: FAKEPACA, not market data" },
];

/** The Live Markets terminal: observed trades and quotes as they arrive, read-only. */
export function LivePage() {
  useLiveConnection();
  const model = useLiveModel();
  const now = useNow();
  const stored = useLive((state) => state.selected);
  const select = useLive((state) => state.select);
  const [windowMs, setWindowMs] = useState("300000");
  const [zone, setZone] = useState<DisplayZone>("eastern");
  const [paused, setPaused] = useState<{ symbol: string; version: number } | null>(null);
  const [visible, setVisible] = useState<Record<SeriesKey, boolean>>({
    trade: true,
    bid: true,
    ask: true,
    mid: true,
  });
  const [diagnostics, setDiagnostics] = useState(false);
  const [feedChoice, setFeedChoice] = useState<FeedName | null>(null);

  const status = model.status;
  const feed = status?.feed;
  const selected =
    stored !== null && model.watchlist.includes(stored) ? stored : (model.watchlist[0] ?? null);
  const state = selected ? model.states[selected] : undefined;
  const elapsed = selected ? now - (model.stateAt.get(selected) ?? now) : 0;
  const ages = agesOf(state, elapsed);
  const banner = headline(model, Math.max(0, now - model.refreshedAt));
  const notices = noticesOf(state, status, ages);
  const buffer = selected ? model.seriesOf(selected) : null;
  const isPaused = paused !== null && paused.symbol === selected;
  const behind = isPaused && buffer ? buffer.version - paused.version : 0;

  const running = feed !== undefined && feed.state !== "STOPPED";
  const chosenFeed: FeedName = running ? feed.feed : (feedChoice ?? feed?.feed ?? "iex");
  const feedOptions =
    feed?.feed === "mock" || chosenFeed === "mock"
      ? [
          ...FEED_OPTIONS,
          { value: "mock" as const, label: "Mock", title: "Local fabricated stream for tests" },
        ]
      : FEED_OPTIONS;

  const control = useMutation({
    mutationFn: ({ path, body }: { path: string; body?: unknown }) => post<unknown>(path, body),
  });
  const offline = model.connection !== "open" || !status;
  const recording = status?.recording;

  return (
    <Page
      context={
        <ContextBar
          crumbs={[{ label: "Live Markets" }, { label: selected ?? "No instrument" }]}
          status={
            feed ? (
              <>
                <span
                  data-testid="source-label"
                  className={cx(
                    "label border px-2 py-0.5",
                    feed.test_data ? "border-warn text-warn" : "border-line-strong text-fg-2",
                  )}
                >
                  {feed.source_label}
                </span>
                <StatusIndicator spec={FEED_STATUS[feed.state]} />
                <StatusIndicator spec={ACTIVITY_STATUS[status.market.activity]} />
              </>
            ) : null
          }
        >
          <SegmentedControl
            label="Feed"
            value={chosenFeed}
            onChange={setFeedChoice}
            options={feedOptions.map((option) => ({ ...option, disabled: running || offline }))}
          />
          {running ? (
            <Button
              size="sm"
              disabled={control.isPending || offline}
              onClick={() => control.mutate({ path: "/live/feed/stop" })}
            >
              Disconnect feed
            </Button>
          ) : (
            <Button
              size="sm"
              variant="primary"
              disabled={control.isPending || offline}
              onClick={() =>
                control.mutate({ path: "/live/feed/start", body: { feed: chosenFeed } })
              }
            >
              Connect feed
            </Button>
          )}
          {recording?.active ? (
            <Button
              size="sm"
              variant="danger"
              disabled={control.isPending || offline}
              onClick={() => control.mutate({ path: "/live/recordings/stop" })}
            >
              Stop recording
            </Button>
          ) : (
            <Button
              size="sm"
              disabled={control.isPending || offline || !running}
              title={running ? undefined : "Connect a feed before recording it"}
              onClick={() => control.mutate({ path: "/live/recordings" })}
            >
              Record
            </Button>
          )}
          <Button size="sm" variant="ghost" onClick={() => setDiagnostics(true)}>
            Diagnostics
          </Button>
        </ContextBar>
      }
    >
      <div className="flex h-full min-h-[720px] flex-col">
        <div
          role="status"
          aria-live="polite"
          data-headline={banner.key}
          className={cx(
            "flex shrink-0 flex-wrap items-baseline gap-x-3 gap-y-0.5 border-b border-l-2 border-b-line bg-surface-1 px-3 py-1.5",
            HEADLINE_TONE[banner.tone],
          )}
        >
          <span className="label">{banner.title}</span>
          <span className="min-w-0 text-xs text-fg-2">{banner.detail}</span>
        </div>
        {control.isError ? <ErrorState error={control.error} compact /> : null}
        <div className="grid min-h-0 flex-1 grid-cols-1 gap-px bg-line-subtle lg:grid-cols-[minmax(0,23rem)_minmax(0,1fr)] lg:grid-rows-[minmax(0,1fr)_minmax(0,19rem)]">
          <InstrumentPanel
            index="01"
            title="Watchlist"
            subtitle={feed ? `${feed.feed.toUpperCase()} · ${model.watchlist.length} symbols` : ""}
            className="max-lg:min-h-[260px]"
          >
            <Watchlist
              model={model}
              now={now}
              selected={selected}
              onSelect={select}
              busy={control.isPending}
              onChange={
                feed && feed.feed !== "test" && !offline
                  ? (symbols) =>
                      control.mutate({ path: "/live/symbols", body: { symbols, feed: feed.feed } })
                  : undefined
              }
            />
          </InstrumentPanel>

          <InstrumentPanel
            index="02"
            title={selected ? `${selected} observed price` : "Observed price"}
            subtitle="source event time · no interpolation"
            className="max-lg:min-h-[460px]"
            bodyClassName="flex flex-col"
          >
            <div
              role="toolbar"
              aria-label="Chart controls"
              className="flex shrink-0 flex-wrap items-center gap-1.5 border-b border-line-subtle bg-surface-1 px-3 py-1"
            >
              {SERIES.map((item) => (
                <ToggleChip
                  key={item.key}
                  label={item.label}
                  swatch={`var(${item.color})`}
                  pressed={visible[item.key]}
                  onChange={(pressed) => setVisible({ ...visible, [item.key]: pressed })}
                />
              ))}
              <SegmentedControl
                label="Time window"
                value={windowMs}
                onChange={setWindowMs}
                options={WINDOWS}
              />
              <SegmentedControl
                label="Display timezone"
                value={zone}
                onChange={setZone}
                options={(Object.keys(ZONE_LABEL) as DisplayZone[]).map((value) => ({
                  value,
                  label: ZONE_LABEL[value],
                }))}
              />
              <Button
                size="sm"
                variant={isPaused ? "primary" : "secondary"}
                disabled={!selected}
                aria-pressed={isPaused}
                onClick={() =>
                  setPaused(
                    isPaused || !selected || !buffer
                      ? null
                      : { symbol: selected, version: buffer.version },
                  )
                }
              >
                {isPaused ? "Follow latest" : "Pause"}
              </Button>
            </div>
            <div className="grid shrink-0 grid-cols-2 gap-x-6 gap-y-2 border-b border-line-subtle px-3 py-2 sm:grid-cols-3 xl:grid-cols-6">
              <MetricReadout
                label="Last trade"
                size="lg"
                value={formatMicroUsd(state?.last_trade?.price_micro_usd)}
                unit="USD"
                note={
                  state?.last_trade
                    ? `${state.last_trade.quantity.toLocaleString("en-US")} @ ${formatEventTime(
                        state.last_trade.timestamp_ns,
                        Math.min(3, state.last_trade.timestamp_precision_digits),
                        zone,
                      )} ${ZONE_LABEL[zone]} · ${formatAge(ages.trade)} ago`
                    : "no trade observed"
                }
              />
              <MetricReadout
                label="Bid"
                value={formatMicroUsd(state?.quote?.bid_micro_usd)}
                note={
                  state?.quote?.bid_size != null
                    ? `size ${state.quote.bid_size} · ${state.quote.bid_venue || UNAVAILABLE}`
                    : state?.quote
                      ? "no bid reported"
                      : "no quote observed"
                }
              />
              <MetricReadout
                label="Ask"
                value={formatMicroUsd(state?.quote?.ask_micro_usd)}
                note={
                  state?.quote?.ask_size != null
                    ? `size ${state.quote.ask_size} · ${state.quote.ask_venue || UNAVAILABLE}`
                    : state?.quote
                      ? "no ask reported"
                      : "no quote observed"
                }
              />
              <MetricReadout
                label="Quoted spread"
                value={formatMicroUsd(state?.spread_micro_usd)}
                note={
                  state?.freshness.spread_quality === "stale_quote"
                    ? "from a stale quote"
                    : state?.freshness.spread_quality === "locked_or_crossed"
                      ? state.quote_state
                      : state?.quote
                        ? `quote ${formatAge(ages.quote)} ago`
                        : UNAVAILABLE
                }
              />
              <MetricReadout
                label="Midpoint"
                value={formatMicroUsd(state?.midpoint_micro_usd)}
                note="of the reported bid and ask"
              />
              <MetricReadout
                label="Source"
                size="sm"
                value={feed ? feed.feed.toUpperCase() : UNAVAILABLE}
                note={feed ? feed.source_label : "no feed"}
              />
            </div>
            {notices.length > 0 ? (
              <ul aria-label="Data qualifications" className="shrink-0 border-b border-line-subtle">
                {notices.map((notice) => (
                  <li
                    key={notice.key}
                    data-notice={notice.key}
                    className="label border-l-2 border-warn bg-surface-1 px-3 py-1 text-warn"
                  >
                    {notice.text}
                  </li>
                ))}
              </ul>
            ) : null}
            <div className="relative min-h-[220px] flex-1">
              {selected ? (
                <LiveChart
                  symbol={selected}
                  windowMs={Number(windowMs)}
                  paused={isPaused}
                  zone={zone}
                  visible={visible}
                />
              ) : null}
              {!buffer || buffer.x.length === 0 ? (
                <p className="pointer-events-none absolute inset-0 flex items-center justify-center px-6 text-center text-sm text-fg-3">
                  {selected
                    ? `No observation of ${selected} has been received in this session. Nothing is drawn until the feed delivers one.`
                    : "Add a symbol to the watchlist."}
                </p>
              ) : null}
              {isPaused ? (
                <p
                  role="status"
                  className="label absolute top-2 left-3 border border-accent-muted bg-surface-1 px-2 py-0.5 text-accent"
                >
                  Paused · {behind.toLocaleString("en-US")} new{" "}
                  {behind === 1 ? "observation" : "observations"} not drawn
                </p>
              ) : null}
            </div>
          </InstrumentPanel>

          <InstrumentPanel
            index="03"
            title={selected ? `${selected} trade and quote tape` : "Trade and quote tape"}
            subtitle={`newest first · ${ZONE_LABEL[zone]} · bounded`}
            className="max-lg:min-h-[320px] lg:col-start-2 lg:row-start-2"
            bodyClassName="overflow-auto"
          >
            <LiveTape rows={selected ? model.tapeOf(selected) : []} zone={zone} />
          </InstrumentPanel>
          <InstrumentPanel
            index="04"
            title="Market status"
            subtitle="connection and activity are separate facts"
            className="max-lg:min-h-[320px] lg:col-start-1 lg:row-start-2"
            bodyClassName="overflow-auto"
          >
            <MarketStatusPanel model={model} now={now} zone={zone} />
          </InstrumentPanel>
        </div>
      </div>
      <FeedDiagnostics
        model={model}
        now={now}
        zone={zone}
        open={diagnostics}
        onOpenChange={setDiagnostics}
      />
    </Page>
  );
}
