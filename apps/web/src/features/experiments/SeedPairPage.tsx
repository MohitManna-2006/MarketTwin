import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { useMemo, useState } from "react";
import { Link, useParams } from "react-router";

import {
  TimeSeriesChart,
  type ScaleSpec,
  type SeriesSpec,
  type XRange,
} from "@/components/charts/TimeSeriesChart";
import { ContextBar } from "@/components/shell/ContextBar";
import { Page } from "@/components/shell/TerminalShell";
import { EventTimeline } from "@/components/timeline/EventTimeline";
import { Button } from "@/components/ui/Button";
import { InstrumentPanel } from "@/components/ui/InstrumentPanel";
import { MetricReadout } from "@/components/ui/MetricReadout";
import { EmptyState, ErrorState, LoadingState } from "@/components/ui/States";
import {
  EXPERIMENT_STATUS,
  RUN_STATUS,
  StatusIndicator,
  type StatusSpec,
} from "@/components/ui/StatusIndicator";
import { useCursorIndex, useRunCursor, useSampleAt } from "@/features/runs/cursor";
import { shockAnnotations } from "@/features/runs/shocks";
import { usePlaybackClock } from "@/hooks/usePlaybackClock";
import { ApiError, post } from "@/lib/api/client";
import { agentSeriesQuery, experimentQuery, marketSeriesQuery, runQuery } from "@/lib/api/queries";
import type { AgentReport, CellRun, ExperimentConfig, RunSummary } from "@/lib/api/types";
import {
  axisCompact,
  axisNumber,
  formatInteger,
  formatMilliticks,
  formatTwiceTicks,
  toneOf,
} from "@/lib/formatting";
import { indexAtOrBefore } from "@/lib/time/asof";
import { usePlayback } from "@/state/playback";

const PRICE: Record<string, ScaleSpec> = {
  y: { format: (value) => axisNumber(value, Number.isInteger(value) ? 0 : 1), minSpan: 4 },
};
const SHARES: Record<string, ScaleSpec> = { y: { format: axisCompact, zero: true, minSpan: 4 } };
const MONEY: Record<string, ScaleSpec> = { y: { format: axisCompact, zero: true, minSpan: 4 } };

const MATCH: StatusSpec = { label: "Digests match the experiment", tone: "positive", glyph: "dot" };
const DIFFER: StatusSpec = {
  label: "Digests differ from the experiment",
  tone: "negative",
  glyph: "diamond",
};

function scaled(column: readonly (number | null)[], divisor: number): (number | null)[] {
  return column.map((value) => (value === null ? null : value / divisor));
}

function Column({
  index,
  runId,
  variant,
  seat,
  subject,
  times,
  xRange,
  onXRange,
  syncKey,
  color,
}: {
  index: string;
  runId: string;
  variant: string;
  /** Which side of the comparison this run is. */
  seat: string;
  subject: number;
  times: readonly number[];
  xRange: XRange | null;
  onXRange: (range: XRange | null) => void;
  syncKey: string;
  color: string;
}) {
  const run = useQuery(runQuery(runId));
  const market = useQuery(marketSeriesQuery(runId));
  const agent = useQuery(agentSeriesQuery(runId, subject));
  const cursor = useCursorIndex();
  const sample = useSampleAt(runId, cursor, String(subject));
  const { events, bands } = useMemo(() => shockAnnotations(run.data?.shocks ?? []), [run.data]);

  const price = useMemo<SeriesSpec[]>(() => {
    const columns = market.data?.columns;
    if (!columns) return [];
    return [
      {
        key: "fundamental",
        label: "Fundamental",
        color: "--chart-fundamental",
        values: scaled(columns.fundamental_milliticks, 1000),
        width: 1,
      },
      {
        key: "mid",
        label: "Midpoint",
        color: "--chart-price",
        values: scaled(columns.mid_twice_ticks, 2),
        width: 1.5,
      },
    ];
  }, [market.data]);
  const inventory = useMemo<SeriesSpec[]>(
    () =>
      agent.data
        ? [
            {
              key: "position",
              label: "Position",
              color,
              values: agent.data.columns.position,
              fill: true,
            },
          ]
        : [],
    [agent.data, color],
  );
  const equity = useMemo<SeriesSpec[]>(
    () =>
      agent.data
        ? [{ key: "equity", label: "Equity", color, values: agent.data.columns.equity_minor }]
        : [],
    [agent.data, color],
  );

  const failure = run.error ?? market.error ?? agent.error;
  const report = run.data?.agents.find((item) => item.id === subject)?.report as
    AgentReport | null | undefined;
  const mine = sample.agents[String(subject)];
  const seek = (ns: number) => {
    usePlayback.getState().pause();
    usePlayback.getState().seek(Math.max(0, indexAtOrBefore(times, ns)));
  };
  const common = { xRange, onXRange, cursorTimes: times, onSeek: seek, syncKey, events, bands };
  const cell = run.data?.cell;

  return (
    <InstrumentPanel
      index={index}
      title={variant}
      subtitle={seat}
      className="min-h-0"
      bodyClassName="flex flex-col"
      actions={
        <>
          {cell && cell.digests_match !== null ? (
            <StatusIndicator spec={cell.digests_match ? MATCH : DIFFER} />
          ) : null}
          <Link
            to={`/runs/${runId}`}
            className="transition-hover label inline-flex h-[22px] items-center rounded-md border border-line bg-surface-1 px-2 text-fg-2 hover:bg-surface-hover hover:text-fg"
          >
            Open in microscope
          </Link>
        </>
      }
    >
      {failure ? (
        <ErrorState error={failure} />
      ) : !market.data || !agent.data ? (
        <LoadingState label="Loading recorded samples" />
      ) : (
        <>
          <div className="grid shrink-0 grid-cols-4 [&>div]:border-b [&>div]:border-line-subtle 2xl:grid-cols-7">
            {(
              [
                [
                  "Mid",
                  formatTwiceTicks(
                    sample.markSource === "Midpoint" ? sample.market?.mark_twice_ticks : null,
                    { fixed: true },
                  ),
                  "ticks",
                  "neutral",
                ],
                [
                  "Fundamental",
                  formatMilliticks(sample.market?.fundamental_milliticks),
                  "ticks",
                  "neutral",
                ],
                ["Spread", formatInteger(sample.market?.spread_ticks), "ticks", "neutral"],
                ["Trades", formatInteger(sample.market?.trade_count), "cum.", "neutral"],
                [
                  "Position",
                  formatInteger(mine?.position, { signed: true }),
                  "sh",
                  toneOf(mine?.position),
                ],
                [
                  "Equity",
                  formatInteger(mine?.equity_minor, { signed: true }),
                  "minor",
                  toneOf(mine?.equity_minor),
                ],
                [
                  "Final P&L",
                  formatInteger(report?.total_pnl_minor, { signed: true }),
                  "minor",
                  toneOf(report?.total_pnl_minor),
                ],
              ] as const
            ).map(([label, value, unit, tone], position) => (
              <div key={label} className="border-r border-line-subtle px-3 py-1.5 last:border-r-0">
                <MetricReadout
                  label={label}
                  value={value}
                  unit={unit}
                  tone={tone}
                  size="sm"
                  note={position === 6 ? "end of run" : position === 0 ? "as of cursor" : undefined}
                />
              </div>
            ))}
          </div>
          <div className="label flex h-5 shrink-0 items-center gap-2 bg-surface-1 px-3 text-fg-3">
            <span className="text-fg-2">Midpoint and fundamental</span> ticks
            <span className="flex-1" />
            <span className="flex items-center gap-1.5">
              <span
                aria-hidden
                className="inline-block h-0.5 w-3"
                style={{ background: "var(--chart-price)" }}
              />
              Mid
            </span>
            <span className="flex items-center gap-1.5">
              <span
                aria-hidden
                className="inline-block h-0.5 w-3"
                style={{ background: "var(--chart-fundamental)" }}
              />
              Fundamental
            </span>
          </div>
          <div className="relative min-h-0 flex-[2.2]">
            <TimeSeriesChart
              {...common}
              x={market.data.columns.timestamp_ns as number[]}
              series={price}
              scales={PRICE}
              showXAxis={false}
              eventLabels
              label={`${variant}: midpoint and fundamental over elapsed simulation time`}
              description="Recorded midpoint and fundamental value at each plotted sample of this run."
            />
          </div>
          <div className="label flex h-5 shrink-0 items-center gap-2 border-t border-line-subtle bg-surface-1 px-3 text-fg-3">
            <span className="text-fg-2">Agent {subject} inventory</span> shares, signed
          </div>
          <div className="relative min-h-0 flex-1">
            <TimeSeriesChart
              {...common}
              x={agent.data.columns.timestamp_ns as number[]}
              series={inventory}
              scales={SHARES}
              showXAxis={false}
              label={`${variant}: agent ${subject} position`}
              description="Recorded signed position of the compared agent at each plotted sample."
            />
          </div>
          <div className="label flex h-5 shrink-0 items-center gap-2 border-t border-line-subtle bg-surface-1 px-3 text-fg-3">
            <span className="text-fg-2">Agent {subject} equity</span> minor units
          </div>
          <div className="relative min-h-0 flex-1">
            <TimeSeriesChart
              {...common}
              x={agent.data.columns.timestamp_ns as number[]}
              series={equity}
              scales={MONEY}
              label={`${variant}: agent ${subject} equity`}
              description="Recorded equity of the compared agent at each plotted sample."
            />
          </div>
        </>
      )}
    </InstrumentPanel>
  );
}

function Pair({
  experimentId,
  seed,
  baseline,
  candidate,
  subject,
}: {
  experimentId: string;
  seed: number;
  baseline: CellRun;
  candidate: CellRun;
  subject: number;
}) {
  const key = `pair-${experimentId}-${seed}`;
  const times = useRunCursor(baseline.run_id, key);
  const other = useRunCursor(candidate.run_id, key);
  usePlaybackClock();
  const [xRange, setXRange] = useState<XRange | null>(null);

  if (times.isPending || other.isPending) return <LoadingState label="Loading recorded samples" />;
  if (times.isError) return <ErrorState error={times.error} />;
  if (other.isError) return <ErrorState error={other.error} />;
  const sampleTimes = times.data.timestamp_ns;
  const aligned =
    sampleTimes.length === other.data.timestamp_ns.length &&
    sampleTimes[sampleTimes.length - 1] ===
      other.data.timestamp_ns[other.data.timestamp_ns.length - 1];

  return (
    <div className="flex flex-col lg:h-full">
      <p className="shrink-0 border-b border-line-subtle bg-surface-0 px-4 py-1.5 text-xs leading-snug text-fg-3">
        <span className="label mr-2 text-fg-2">Linked by elapsed simulation time</span>
        Both runs use seed <span className="num text-fg-2">{seed}</span> and share every exogenous
        input: the fundamental path, wakeup times, and each trader&apos;s random draws. They are not
        the same market: once agent {subject} quotes differently, who trades with whom diverges, and
        the two price paths are separate histories.
        {aligned
          ? ""
          : " The two runs recorded different sample times; the cursor follows the baseline's."}
      </p>
      <div className="hairline-grid grid min-h-0 flex-1 grid-cols-1 max-lg:auto-rows-[760px] lg:grid-cols-2">
        <Column
          index="01"
          runId={baseline.run_id}
          variant={baseline.variant}
          seat="baseline"
          subject={subject}
          times={sampleTimes}
          xRange={xRange}
          onXRange={setXRange}
          syncKey={key}
          color="--chart-baseline"
        />
        <Column
          index="02"
          runId={candidate.run_id}
          variant={candidate.variant}
          seat="candidate"
          subject={subject}
          times={sampleTimes}
          xRange={xRange}
          onXRange={setXRange}
          syncKey={key}
          color="--chart-inventory-aware"
        />
      </div>
      <EventTimeline times={sampleTimes} xRange={xRange} note="shared by both runs" />
    </div>
  );
}

export function SeedPairPage() {
  const { experimentId = "", seed: seedParam = "" } = useParams();
  const seed = Number(seedParam);
  const client = useQueryClient();
  const query = useQuery({
    ...experimentQuery(experimentId),
    refetchInterval: (state) => {
      const data = state.state.data;
      if (!data) return false;
      const busy = data.cell_runs.some(
        (cell) => cell.seed === seed && (cell.status === "pending" || cell.status === "running"),
      );
      return busy || data.status === "pending" || data.status === "running" ? 500 : false;
    },
  });
  const experiment = query.data;
  const config = experiment?.config as unknown as ExperimentConfig | undefined;
  const comparison = config?.comparison ?? null;

  const materialize = useMutation({
    mutationFn: async (variants: string[]) => {
      for (const variant of variants) {
        await post<RunSummary>(`/experiments/${experimentId}/cells/materialize`, { variant, seed });
      }
    },
    onSettled: () => void client.invalidateQueries({ queryKey: ["experiments", experimentId] }),
  });

  const context = (
    <ContextBar
      crumbs={[
        { label: "Experiment", to: "/experiments" },
        {
          label: experiment?.name ?? experimentId,
          to: `/experiments/${experimentId}`,
          verbatim: true,
        },
        { label: `Seed ${seedParam}` },
        ...(comparison
          ? [{ label: `${comparison.candidate} vs ${comparison.baseline}`, verbatim: true }]
          : []),
      ]}
      status={
        experiment ? <StatusIndicator spec={EXPERIMENT_STATUS[experiment.status]} /> : undefined
      }
    />
  );

  let body;
  if (query.isPending) body = <LoadingState label="Loading experiment" />;
  else if (query.isError)
    body = <ErrorState error={query.error} onRetry={() => void query.refetch()} />;
  else if (!experiment || !comparison || !Number.isSafeInteger(seed)) {
    body = (
      <EmptyState title="No pair to inspect">
        This experiment defines no baseline and candidate, so a seed has no pair of runs.
      </EmptyState>
    );
  } else {
    const find = (variant: string) =>
      experiment.cell_runs.find(
        (cell) => cell.variant === variant && cell.seed === seed && cell.status !== "failed",
      );
    const baseline = find(comparison.baseline);
    const candidate = find(comparison.candidate);
    const missing = [comparison.baseline, comparison.candidate].filter((variant) => !find(variant));
    if (baseline?.status === "completed" && candidate?.status === "completed") {
      body = (
        <Pair
          experimentId={experimentId}
          seed={seed}
          baseline={baseline}
          candidate={candidate}
          subject={comparison.subject_agent_id}
        />
      );
    } else if (missing.length === 0 || materialize.isPending) {
      body = (
        <div className="h-full bg-surface-0">
          <EmptyState title="Regenerating the two runs">
            <span className="flex flex-col items-center gap-2">
              <span>
                The engine is rerunning the exact config of each cell. Each result is then checked
                against the digests the experiment recorded for it.
              </span>
              <span className="flex gap-4">
                {[baseline, candidate].map((cell, index) =>
                  cell ? (
                    <StatusIndicator
                      key={index}
                      spec={RUN_STATUS[cell.status]}
                      label={`${cell.variant}: ${cell.status}`}
                    />
                  ) : null,
                )}
              </span>
            </span>
          </EmptyState>
        </div>
      );
    } else {
      body = (
        <div className="h-full bg-surface-0">
          <EmptyState
            title={`The runs of seed ${seed} were not kept`}
            actions={
              <Button variant="solid" onClick={() => materialize.mutate(missing)}>
                Regenerate and inspect
              </Button>
            }
          >
            <span>
              This experiment recorded each run&apos;s digests instead of its artifacts. The engine
              gives a cell the same digests wherever it runs, so the terminal can regenerate{" "}
              <span className="num text-fg-2">{missing.join(" and ")}</span> for this seed as
              ordinary runs and verify them against the experiment&apos;s record.
            </span>
            {materialize.isError ? (
              <span role="alert" className="mt-2 block text-neg">
                {materialize.error instanceof ApiError
                  ? `${materialize.error.code}: ${materialize.error.message}`
                  : "The research API did not respond."}
              </span>
            ) : null}
          </EmptyState>
        </div>
      );
    }
  }

  return <Page context={context}>{body}</Page>;
}
