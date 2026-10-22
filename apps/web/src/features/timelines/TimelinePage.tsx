import { useMutation, useQuery } from "@tanstack/react-query";
import { useMemo, useState } from "react";
import { Link, useNavigate } from "react-router";

import {
  TimeSeriesChart,
  type ScaleSpec,
  type SeriesSpec,
  type XRange,
} from "@/components/charts/TimeSeriesChart";
import { Button } from "@/components/ui/Button";
import { SelectField } from "@/components/ui/Field";
import { InstrumentPanel } from "@/components/ui/InstrumentPanel";
import { KeyValue } from "@/components/ui/KeyValue";
import { EmptyState, ErrorState, LoadingState } from "@/components/ui/States";
import { useRun } from "@/features/runs/RunLayout";
import { post } from "@/lib/api/client";
import {
  agentSeriesQuery,
  divergenceQuery,
  marketSeriesQuery,
  timelineQuery,
} from "@/lib/api/queries";
import type {
  Compared,
  DeskInfo,
  Divergence,
  ExperimentSummary,
  ForkReport,
  RunDetail,
  TimelineNode,
} from "@/lib/api/types";
import { cx } from "@/lib/cx";
import { axisCompact, axisNumber, formatElapsed, formatInteger } from "@/lib/formatting";

import { ForkForm } from "./ForkForm";
import { TimelineTree, changeSummary } from "./TimelineTree";

const PRICE: Record<string, ScaleSpec> = {
  price: { format: (value) => axisNumber(value, Number.isInteger(value) ? 0 : 1), minSpan: 4 },
};
const GAP: Record<string, ScaleSpec> = {
  y: { format: (value) => axisNumber(value, 1), zero: true, minSpan: 1 },
};
const MONEY: Record<string, ScaleSpec> = { y: { format: axisCompact, zero: true, minSpan: 4 } };

const VALUE_LABELS: Record<string, string> = {
  equity_minor: "Equity",
  realized_pnl_minor: "Realized P&L",
  unrealized_pnl_minor: "Unrealized P&L",
  position: "Position",
  cash_minor: "Cash",
  cost_basis_minor: "Cost basis",
  total_pnl_minor: "Total P&L",
  max_drawdown_minor: "Max drawdown",
  mean_abs_position_millishares: "Mean |position| (millishares)",
  fills: "Fills",
};

function signed(value: number | null): string {
  if (value === null) return "—";
  return `${value > 0 ? "+" : ""}${formatInteger(value)}`;
}

/** Fork values on the parent's sample times, as plot coordinates. */
function aligned(
  times: readonly (number | null)[],
  other: readonly (number | null)[],
  values: readonly (number | null)[],
  divisor: number,
): (number | null)[] {
  const byTime = new Map<number, number | null>();
  other.forEach((time, index) => {
    if (time !== null) byTime.set(time, values[index] ?? null);
  });
  return times.map((time) => {
    const value = time === null ? undefined : byTime.get(time);
    return value === undefined || value === null ? null : value / divisor;
  });
}

function RecordLine({ label, record }: { label: string; record: Divergence["parent"] }) {
  return (
    <div className="flex flex-col gap-0.5">
      <span className="label text-fg-3">{label}</span>
      {record ? (
        <span className="num text-sm text-fg">
          {record.kind}
          {record.actor_id !== null ? ` · agent ${record.actor_id}` : ""} · seq{" "}
          {formatInteger(record.sequence)}
        </span>
      ) : (
        <span className="text-sm text-fg-3">nothing: the run had already ended</span>
      )}
    </div>
  );
}

function DivergenceCard({ title, divergence }: { title: string; divergence: Divergence | null }) {
  return (
    <div className="flex flex-col gap-2 border-b border-line-subtle p-3">
      <div className="label text-fg-2">{title}</div>
      {divergence ? (
        <>
          <div className="num text-md text-warn">
            {formatElapsed(divergence.timestamp_ns, "ms")}
          </div>
          <RecordLine label="Parent" record={divergence.parent} />
          <RecordLine label="Fork" record={divergence.fork} />
        </>
      ) : (
        <p className="text-sm text-fg-2">
          None before the horizon: on this path the change made no observable difference.
        </p>
      )}
    </div>
  );
}

function subjectOf(node: TimelineNode | undefined, run: RunDetail): number {
  const stage = node?.stage;
  return (
    stage?.policy_overrides?.[0]?.agent_id ??
    stage?.directive_overrides?.[0]?.agent_id ??
    (run.desk as DeskInfo | null | undefined)?.agents[0] ??
    (run.config as { exchange: { agents: number[] } }).exchange.agents[0] ??
    1
  );
}

function Comparison({ run, report }: { run: RunDetail; report: ForkReport }) {
  const parent = run.parent_run_id ?? "";
  const [xRange, setXRange] = useState<XRange | null>(null);
  const agents = report.agents.map((item) => item.agent_id);
  const timeline = useQuery(timelineQuery(run.id));
  const node = timeline.data?.nodes.find((item) => item.id === run.id);
  const [agent, setAgent] = useState<number | null>(null);
  const subject = agent ?? subjectOf(node, run);
  const mine = useQuery(marketSeriesQuery(run.id));
  const theirs = useQuery(marketSeriesQuery(parent));
  const myAgent = useQuery(agentSeriesQuery(run.id, subject));
  const theirAgent = useQuery(agentSeriesQuery(parent, subject));
  const at = report.at_ns;
  const first = report.divergence.first?.timestamp_ns ?? null;
  const events = [
    { ns: at, label: "Fork" },
    ...(first !== null ? [{ ns: first, label: "First divergence" }] : []),
  ];
  const bands = [{ from: 0, to: at }];

  const price = useMemo(() => {
    if (!mine.data || !theirs.data) return null;
    const times = theirs.data.columns.timestamp_ns;
    const series: SeriesSpec[] = [
      {
        key: "parent",
        label: "Parent mid",
        color: "--chart-baseline",
        values: theirs.data.columns.mid_twice_ticks.map((value) =>
          value === null ? null : value / 2,
        ),
        scale: "price",
      },
      {
        key: "fork",
        label: "Fork mid",
        color: "--chart-price",
        values: aligned(
          times,
          mine.data.columns.timestamp_ns,
          mine.data.columns.mid_twice_ticks,
          2,
        ),
        scale: "price",
        width: 1.5,
      },
    ];
    return { x: times as number[], series };
  }, [mine.data, theirs.data]);

  const equity = useMemo(() => {
    if (!myAgent.data || !theirAgent.data) return null;
    const times = theirAgent.data.columns.timestamp_ns;
    const series: SeriesSpec[] = [
      {
        key: "parent",
        label: "Parent equity",
        color: "--chart-baseline",
        values: theirAgent.data.columns.equity_minor,
      },
      {
        key: "fork",
        label: "Fork equity",
        color: "--chart-price",
        values: aligned(
          times,
          myAgent.data.columns.timestamp_ns,
          myAgent.data.columns.equity_minor,
          1,
        ),
        width: 1.5,
      },
    ];
    return { x: times as number[], series };
  }, [myAgent.data, theirAgent.data]);

  const gap = useMemo(() => {
    const samples = report.market.samples;
    return {
      x: samples.map((item) => item.timestamp_ns),
      series: [
        {
          key: "gap",
          label: "|fork mid − parent mid|",
          color: "--data-warning",
          values: samples.map((item) =>
            item.mid_gap_milliticks === null ? null : item.mid_gap_milliticks / 1000,
          ),
          fill: true,
        },
      ] satisfies SeriesSpec[],
    };
  }, [report]);

  return (
    <div className="flex min-h-0 flex-col gap-px">
      <InstrumentPanel
        index="02"
        title="Parent and fork"
        subtitle="shaded: the shared past, identical record for record"
        unit="ticks"
      >
        <div className="relative h-56">
          {price ? (
            <TimeSeriesChart
              x={price.x}
              series={price.series}
              scales={PRICE}
              xRange={xRange}
              onXRange={setXRange}
              syncKey={`fork-${run.id}`}
              events={events}
              bands={bands}
              eventLabels
              showXAxis={false}
              label="Midpoint of the parent and the fork over elapsed simulation time"
              description={`The parent's midpoint and the fork's on the same sample times. They are identical until the fork at ${formatElapsed(at, "s")}.`}
            />
          ) : mine.isError || theirs.isError ? (
            <ErrorState error={mine.error ?? theirs.error} compact />
          ) : (
            <LoadingState label="Loading both markets" />
          )}
        </div>
      </InstrumentPanel>
      <InstrumentPanel
        index="03"
        title="How far the futures part"
        subtitle="absolute midpoint gap at each sample after the fork"
        unit="ticks"
      >
        <div className="relative h-28">
          <TimeSeriesChart
            x={gap.x}
            series={gap.series}
            scales={GAP}
            xRange={xRange}
            onXRange={setXRange}
            syncKey={`fork-${run.id}`}
            events={events}
            label="Gap between the fork's midpoint and the parent's"
            description="Zero means the two markets quote the same midpoint at that sample."
          />
        </div>
      </InstrumentPanel>
      <InstrumentPanel
        index="04"
        title={`Agent ${subject} equity`}
        unit="minor"
        actions={
          <div className="w-40">
            <SelectField
              label="Agent"
              inline
              value={String(subject)}
              onChange={(value) => setAgent(Number(value))}
              options={agents.map((id) => ({ value: String(id), label: `Agent ${id}` }))}
            />
          </div>
        }
      >
        <div className="relative h-40">
          {equity ? (
            <TimeSeriesChart
              x={equity.x}
              series={equity.series}
              scales={MONEY}
              xRange={xRange}
              onXRange={setXRange}
              syncKey={`fork-${run.id}`}
              events={events}
              bands={bands}
              label={`Equity of agent ${subject} in the parent and the fork`}
              description="Marked-to-market equity in minor currency units, from each run's own accounting."
            />
          ) : (
            <LoadingState label="Loading agent series" />
          )}
        </div>
      </InstrumentPanel>
    </div>
  );
}

function Differences({ report }: { report: ForkReport }) {
  const window = report.window;
  const rows = window
    ? window.agents.map((item) => ({ agent: item.agent_id, values: item.values }))
    : report.agents.map((item) => ({ agent: item.agent_id, values: item.values }));
  const keys = window
    ? ["equity_minor", "realized_pnl_minor", "position"]
    : ["total_pnl_minor", "max_drawdown_minor", "position"];
  return (
    <table className="w-full border-collapse text-left text-sm">
      <thead className="bg-surface-1">
        <tr className="label text-fg-3">
          <th className="border-b border-line px-3 py-1.5 font-medium">Agent</th>
          {keys.map((key) => (
            <th key={key} className="border-b border-line px-3 py-1.5 text-right font-medium">
              {VALUE_LABELS[key] ?? key} Δ
            </th>
          ))}
        </tr>
      </thead>
      <tbody>
        {rows.map((row) => (
          <tr key={row.agent} className="border-b border-line-subtle">
            <td className="num px-3 py-1 text-fg-2">{row.agent}</td>
            {keys.map((key) => {
              const value: Compared | undefined = row.values[key];
              const difference = value?.difference ?? null;
              return (
                <td
                  key={key}
                  title={`parent ${formatInteger(value?.parent)} · fork ${formatInteger(value?.fork)}`}
                  className={cx(
                    "num px-3 py-1 text-right",
                    difference === null || difference === 0
                      ? "text-fg-3"
                      : difference > 0
                        ? "text-pos"
                        : "text-neg",
                  )}
                >
                  {signed(difference)}
                </td>
              );
            })}
          </tr>
        ))}
      </tbody>
    </table>
  );
}

function Verification({ run, report }: { run: RunDetail; report: ForkReport }) {
  const navigate = useNavigate();
  const repeat = useMutation({
    mutationFn: () => post<ExperimentSummary>(`/runs/${run.id}/repeat`, { seeds: 20 }),
    onSuccess: (experiment) => void navigate(`/experiments/${experiment.id}`),
  });
  return (
    <>
      <div className="flex flex-col gap-1 border-b border-line-subtle border-l-4 border-l-pos bg-surface-1 p-3">
        <div className="label text-sm text-pos">
          Prefix verified · {formatInteger(report.prefix.records)} records identical
        </div>
        <p className="text-xs text-fg-3">
          Both runs replay byte for byte, the fork names its parent&apos;s config digest, and every
          record before {formatElapsed(report.at_ns, "ms")} matches the parent&apos;s.
        </p>
        <div className="label text-xs text-warn">{report.label}</div>
      </div>
      <DivergenceCard title="First divergence" divergence={report.divergence.first} />
      <DivergenceCard title="First market divergence" divergence={report.divergence.first_market} />
      <KeyValue
        rows={[
          { label: "Trades", value: signed(report.market.trades.difference) },
          { label: "Events", value: signed(report.market.events.difference) },
          {
            label: "Compared to",
            value: report.window ? formatElapsed(report.window.end_ns, "s") : "the horizon",
          },
          {
            label: "Prefix digest",
            value: <span className="num text-xs">{report.prefix.digest.slice(0, 16)}…</span>,
          },
        ]}
      />
      <div className="flex flex-col gap-2 p-3">
        <Button disabled={repeat.isPending} onClick={() => repeat.mutate()}>
          {repeat.isPending ? "Starting…" : "Repeat over 20 seeds"}
        </Button>
        <p className="text-xs text-fg-3">
          The same fork on 20 seeds as a paired experiment: every pair shares its history up to the
          fork by construction.
        </p>
        {repeat.isError ? <ErrorState error={repeat.error} compact /> : null}
      </div>
    </>
  );
}

export function TimelinePage() {
  const run = useRun();
  const timeline = useQuery(timelineQuery(run.id));
  const forked = Boolean(run.parent_run_id);
  const divergence = useQuery(divergenceQuery(run.id, forked && run.fork_status !== null));
  const report = divergence.data?.report ?? null;
  const node = timeline.data?.nodes.find((item) => item.id === run.id);
  return (
    <div className="grid min-h-full grid-cols-1 gap-px bg-line-subtle xl:grid-cols-[minmax(0,1fr)_minmax(0,26rem)]">
      <div className="flex min-w-0 flex-col gap-px">
        <InstrumentPanel
          index="01"
          title="Timelines"
          subtitle={node ? changeSummary(node) : undefined}
        >
          {timeline.data ? (
            <div className="p-2">
              <TimelineTree timeline={timeline.data} />
            </div>
          ) : timeline.isError ? (
            <ErrorState error={timeline.error} compact />
          ) : (
            <LoadingState label="Loading timelines" />
          )}
        </InstrumentPanel>
        {forked && report ? (
          <Comparison run={run} report={report} />
        ) : forked && run.fork_status === "failed" ? (
          <EmptyState title="The fork did not verify">
            {divergence.data ? `${divergence.data.code}: ${divergence.data.message}` : null}
          </EmptyState>
        ) : forked ? (
          <LoadingState label="Loading the fork report" />
        ) : (
          <div className="bg-surface-0">
            <EmptyState title="A root run">
              Fork it from any moment: the engine replays the identical past and branches a new
              future, then proves the two agree up to the fork.
            </EmptyState>
          </div>
        )}
      </div>
      <div className="flex min-w-0 flex-col gap-px">
        {forked && report ? (
          <InstrumentPanel
            index="05"
            title="Verified fork"
            subtitle={
              run.parent_run_id ? (
                <Link to={`/runs/${run.parent_run_id}/timeline`} className="underline">
                  parent {run.parent_run_id}
                </Link>
              ) : undefined
            }
          >
            <Verification run={run} report={report} />
          </InstrumentPanel>
        ) : null}
        {forked && report ? (
          <InstrumentPanel
            index="06"
            title="Fork minus parent"
            subtitle={report.window ? "at the end of the shared window" : "at the horizon"}
            unit="minor / shares"
          >
            <Differences report={report} />
          </InstrumentPanel>
        ) : null}
        <InstrumentPanel index="07" title="Fork this run">
          <ForkForm run={run} at={report ? report.at_ns : Math.floor(run.horizon_ns / 2)} />
        </InstrumentPanel>
      </div>
    </div>
  );
}
