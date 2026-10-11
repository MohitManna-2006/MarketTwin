import { useQuery } from "@tanstack/react-query";
import { useEffect, useMemo, useState } from "react";
import { useNavigate, useSearchParams } from "react-router";
import { Panel, PanelGroup, PanelResizeHandle } from "react-resizable-panels";

import type { XRange } from "@/components/charts/TimeSeriesChart";
import { EventTimeline } from "@/components/timeline/EventTimeline";
import { Button } from "@/components/ui/Button";
import { InstrumentPanel } from "@/components/ui/InstrumentPanel";
import { EmptyState, ErrorBoundary, ErrorState, LoadingState } from "@/components/ui/States";
import { useCursorIndex, useRunCursor, useSampleAt } from "@/features/runs/cursor";
import { useRun } from "@/features/runs/RunLayout";
import { shockAnnotations } from "@/features/runs/shocks";
import { SegmentedControl } from "@/components/ui/SegmentedControl";
import { useDebounced } from "@/hooks/useDebounced";
import { useMediaQuery } from "@/hooks/useMediaQuery";
import { usePlaybackClock } from "@/hooks/usePlaybackClock";
import { eventsQuery } from "@/lib/api/queries";
import type { RunDetail } from "@/lib/api/types";
import { formatDuration } from "@/lib/formatting";
import { indexAtOrBefore } from "@/lib/time/asof";
import { usePlayback } from "@/state/playback";

import { AgentTable } from "./AgentTable";
import { EventStream } from "./EventStream";
import { MarketTelemetry } from "./MarketTelemetry";
import { OrderBookLadder } from "./OrderBookLadder";
import { PricePanel } from "./PricePanel";

function Handle({ direction }: { direction: "horizontal" | "vertical" }) {
  return (
    <PanelResizeHandle
      className={
        direction === "horizontal"
          ? "transition-hover w-px bg-line-subtle hover:bg-accent-muted data-[resize-handle-state=drag]:bg-accent focus-visible:bg-focus"
          : "transition-hover h-px bg-line-subtle hover:bg-accent-muted data-[resize-handle-state=drag]:bg-accent focus-visible:bg-focus"
      }
    />
  );
}

/** Panels that read the cursor, so only they re-render as playback advances. */
function CursorPanels({
  run,
  selectedAgent,
  onSelectAgent,
  part,
}: {
  run: RunDetail;
  selectedAgent: number | null;
  onSelectAgent: (agent: number | null) => void;
  part: "book" | "telemetry" | "agents";
}) {
  const index = useCursorIndex();
  const sample = useSampleAt(run.id, index, run.agents.length <= 64 ? "all" : "none");
  if (sample.error) return <ErrorState error={sample.error} compact />;
  if (part === "book") {
    const depth =
      (run.sampling as { depth?: number } | null)?.depth ?? sample.market?.depth_levels ?? null;
    return <OrderBookLadder book={sample.book} depth={depth} isPending={sample.isPending} />;
  }
  if (part === "telemetry") return <MarketTelemetry run={run} sample={sample} />;
  return <AgentTable run={run} sample={sample} selected={selectedAgent} onSelect={onSelectAgent} />;
}

function StreamPanel({
  run,
  selectedAgent,
  times,
}: {
  run: RunDetail;
  selectedAgent: number | null;
  times: readonly number[];
}) {
  const index = useCursorIndex();
  const kinds = useQuery({
    ...eventsQuery(run.id, { stream: "market" }, 0, 1),
    select: (page) => page.kinds,
  });
  return (
    <EventStream
      runId={run.id}
      cursorNs={times[index] ?? null}
      agentId={selectedAgent}
      kinds={kinds.data ?? []}
      onSeekTime={(ns) => {
        usePlayback.getState().pause();
        usePlayback.getState().seek(Math.max(0, indexAtOrBefore(times, ns)));
      }}
    />
  );
}

/** Keeps `?t=` in step with the cursor while it rests, so a view can be shared. */
function useCursorInUrl(times: readonly number[] | undefined) {
  const [, setParams] = useSearchParams();
  const index = usePlayback((state) => state.index);
  const playing = usePlayback((state) => state.playing);
  const settled = useDebounced(index, 400);
  useEffect(() => {
    if (!times || playing) return;
    const ns = times[settled];
    if (ns === undefined) return;
    setParams(
      (current) => {
        if (current.get("t") === String(ns)) return current;
        const next = new URLSearchParams(current);
        next.set("t", String(ns));
        return next;
      },
      { replace: true },
    );
  }, [settled, playing, times, setParams]);
}

export function MicroscopePage() {
  const run = useRun();
  const navigate = useNavigate();
  const times = useRunCursor(run.id);
  usePlaybackClock();
  const [xRange, setXRange] = useState<XRange | null>(null);
  const wide = useMediaQuery("(min-width: 900px)");
  const [tab, setTab] = useState<"price" | "book" | "telemetry" | "agents" | "stream">("price");
  const [params, setParams] = useSearchParams();
  const agentParam = params.get("agent");
  const selectedAgent = agentParam !== null && /^\d+$/.test(agentParam) ? Number(agentParam) : null;
  const { events, bands } = useMemo(() => shockAnnotations(run.shocks), [run.shocks]);
  useCursorInUrl(times.data?.timestamp_ns);

  function selectAgent(agent: number | null) {
    setParams(
      (current) => {
        const next = new URLSearchParams(current);
        if (agent === null) next.delete("agent");
        else next.set("agent", String(agent));
        return next;
      },
      { replace: true },
    );
  }

  if (times.isPending) return <LoadingState label="Loading recorded samples" />;
  if (times.isError) {
    return (
      <div className="h-full bg-surface-0">
        <ErrorState error={times.error} onRetry={() => void times.refetch()} />
      </div>
    );
  }
  const sampleTimes = times.data.timestamp_ns;
  const sampling = run.sampling as { interval_ns: number; depth: number } | null;
  const noSamples = sampleTimes.length === 0;

  // On a narrow screen the workspace is one panel at a time, chosen by tab: a chart or a
  // ladder squeezed beside four other panels would be unreadable.
  if (!wide) {
    return (
      <div className="flex h-full min-h-[560px] flex-col">
        <div className="flex h-8 shrink-0 items-center overflow-x-auto border-b border-line bg-surface-1 px-2">
          <SegmentedControl
            label="Panel"
            size="md"
            value={tab}
            onChange={setTab}
            options={[
              { value: "price", label: "Price" },
              { value: "book", label: "Book" },
              { value: "telemetry", label: "Telemetry" },
              { value: "agents", label: "Agents" },
              { value: "stream", label: "Events" },
            ]}
          />
        </div>
        <div className="flex min-h-0 flex-1 flex-col">
          {tab === "price" ? (
            <PricePanel
              run={run}
              times={sampleTimes}
              selectedAgent={selectedAgent}
              xRange={xRange}
              onXRange={setXRange}
            />
          ) : tab === "stream" ? (
            <InstrumentPanel index="04" title="Execution stream" className="flex-1">
              <StreamPanel run={run} selectedAgent={selectedAgent} times={sampleTimes} />
            </InstrumentPanel>
          ) : (
            <InstrumentPanel
              index={tab === "book" ? "02" : tab === "telemetry" ? "03" : "05"}
              title={
                tab === "book" ? "Order book" : tab === "telemetry" ? "Market telemetry" : "Agents"
              }
              subtitle={tab === "telemetry" ? undefined : "as of the cursor"}
              className="flex-1"
            >
              {noSamples && tab !== "telemetry" ? (
                <EmptyState title="Nothing recorded on a schedule">
                  This run took no samples, so there is no depth or account state to show.
                </EmptyState>
              ) : (
                <CursorPanels
                  run={run}
                  selectedAgent={selectedAgent}
                  onSelectAgent={selectAgent}
                  part={tab}
                />
              )}
            </InstrumentPanel>
          )}
        </div>
        <EventTimeline times={sampleTimes} events={events} bands={bands} xRange={xRange} />
      </div>
    );
  }

  return (
    <div className="flex h-full flex-col">
      <PanelGroup direction="horizontal" autoSaveId="microscope-columns" className="min-h-0 flex-1">
        <Panel defaultSize={74} minSize={45} className="flex min-w-0 flex-col">
          <PanelGroup direction="vertical" autoSaveId="microscope-rows" className="min-h-0 flex-1">
            <Panel defaultSize={62} minSize={30} className="flex min-h-0 flex-col">
              <ErrorBoundary label="Market price" resetKey={run.id}>
                <PricePanel
                  run={run}
                  times={sampleTimes}
                  selectedAgent={selectedAgent}
                  xRange={xRange}
                  onXRange={setXRange}
                />
              </ErrorBoundary>
            </Panel>
            <Handle direction="vertical" />
            <Panel defaultSize={38} minSize={16} className="flex min-h-0">
              <PanelGroup direction="horizontal" autoSaveId="microscope-lower-v2">
                <Panel defaultSize={66} minSize={30} className="flex min-w-0">
                  <InstrumentPanel
                    index="04"
                    title="Execution stream"
                    subtitle="commands and outcomes in dispatch order"
                    className="flex-1"
                  >
                    <ErrorBoundary label="Execution stream" resetKey={run.id}>
                      <StreamPanel run={run} selectedAgent={selectedAgent} times={sampleTimes} />
                    </ErrorBoundary>
                  </InstrumentPanel>
                </Panel>
                <Handle direction="horizontal" />
                <Panel defaultSize={34} minSize={20} className="flex min-w-0">
                  <InstrumentPanel
                    index="05"
                    title="Agents"
                    subtitle="accounts as of the cursor"
                    className="flex-1"
                    actions={
                      <Button
                        size="sm"
                        variant="ghost"
                        disabled={selectedAgent === null}
                        onClick={() => void navigate(`/runs/${run.id}/agents/${selectedAgent}`)}
                      >
                        Inspect agent
                      </Button>
                    }
                  >
                    {noSamples ? (
                      <EmptyState title="No account samples">
                        Accounts are recorded on the sampling schedule, and this run has none. Final
                        balances are in the Agent Inspector.
                      </EmptyState>
                    ) : (
                      <CursorPanels
                        run={run}
                        selectedAgent={selectedAgent}
                        onSelectAgent={selectAgent}
                        part="agents"
                      />
                    )}
                  </InstrumentPanel>
                </Panel>
              </PanelGroup>
            </Panel>
          </PanelGroup>
        </Panel>
        <Handle direction="horizontal" />
        <Panel defaultSize={26} minSize={18} maxSize={40} className="flex min-w-0 flex-col">
          <PanelGroup direction="vertical" autoSaveId="microscope-side">
            <Panel defaultSize={44} minSize={24} className="flex min-h-0">
              <InstrumentPanel
                index="02"
                title="Order book"
                subtitle="recorded depth snapshot"
                className="flex-1"
              >
                {noSamples ? (
                  <EmptyState title="No depth recorded">
                    Depth is written to the trace only at sampling times. Enable sampling in the
                    Simulation Lab to record it.
                  </EmptyState>
                ) : (
                  <CursorPanels
                    run={run}
                    selectedAgent={selectedAgent}
                    onSelectAgent={selectAgent}
                    part="book"
                  />
                )}
              </InstrumentPanel>
            </Panel>
            <Handle direction="vertical" />
            <Panel defaultSize={56} minSize={24} className="flex min-h-0">
              <InstrumentPanel index="03" title="Market telemetry" className="flex-1">
                <CursorPanels
                  run={run}
                  selectedAgent={selectedAgent}
                  onSelectAgent={selectAgent}
                  part="telemetry"
                />
              </InstrumentPanel>
            </Panel>
          </PanelGroup>
        </Panel>
      </PanelGroup>
      <EventTimeline
        times={sampleTimes}
        events={events}
        bands={bands}
        xRange={xRange}
        note={
          sampling
            ? `one sample every ${formatDuration(sampling.interval_ns)}`
            : "scripted snapshots"
        }
      />
    </div>
  );
}
