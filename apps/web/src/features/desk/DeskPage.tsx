import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { useMemo, useState } from "react";
import { Link, useNavigate } from "react-router";

import {
  TimeSeriesChart,
  type ScaleSpec,
  type SeriesSpec,
  type XRange,
} from "@/components/charts/TimeSeriesChart";
import { Button } from "@/components/ui/Button";
import { InstrumentPanel } from "@/components/ui/InstrumentPanel";
import { KeyValue } from "@/components/ui/KeyValue";
import { EmptyState, ErrorState, LoadingState } from "@/components/ui/States";
import { useRun } from "@/features/runs/RunLayout";
import { post } from "@/lib/api/client";
import {
  agentSeriesQuery,
  decisionLedgerQuery,
  decisionsQuery,
  marketSeriesQuery,
} from "@/lib/api/queries";
import type {
  DecisionLedger,
  DeskInfo,
  DeskDecisionRow,
  Directive,
  ExperimentSummary,
  LedgerEntry,
} from "@/lib/api/types";
import { cx } from "@/lib/cx";
import { axisCompact, axisNumber, formatElapsed, formatInteger } from "@/lib/formatting";

// The AI desk (ADR 0012): one agent whose directives a language model chose. The model
// never sent an order; the execution layer did, and the book decided every fill. The
// decision ledger forks the run at each decision against the directive it replaced.

const PRICE: Record<string, ScaleSpec> = {
  price: { format: (value) => axisNumber(value, Number.isInteger(value) ? 0 : 1), minSpan: 4 },
};
const MONEY: Record<string, ScaleSpec> = { y: { format: axisCompact, zero: true, minSpan: 4 } };

const MODE_CLASS: Record<Directive["mode"], string> = {
  quote: "border-info text-info",
  pause: "border-warn text-warn",
  reduce: "border-neg text-neg",
};

export function DirectiveChip({ directive }: { directive: Directive }) {
  return (
    <span className="inline-flex items-center gap-1.5">
      <span
        className={cx(
          "label rounded-sm border px-1 text-2xs leading-4",
          MODE_CLASS[directive.mode],
        )}
      >
        {directive.mode}
      </span>
      {directive.mode === "quote" ? (
        <span className="num text-xs text-fg-2">
          ±{directive.half_spread_ticks}t skew {directive.skew_ticks > 0 ? "+" : ""}
          {directive.skew_ticks} · {directive.levels}×{directive.quote_size} · cap{" "}
          {formatInteger(directive.max_position)}
        </span>
      ) : null}
    </span>
  );
}

function sameDirective(left: Directive | undefined, right: Directive): boolean {
  if (!left) return false;
  return (
    left.mode === right.mode &&
    left.half_spread_ticks === right.half_spread_ticks &&
    left.skew_ticks === right.skew_ticks &&
    left.quote_size === right.quote_size &&
    left.levels === right.levels &&
    left.max_position === right.max_position
  );
}

function signed(value: number | null): string {
  if (value === null) return "—";
  return `${value > 0 ? "+" : ""}${formatInteger(value)}`;
}

function LedgerBars({ ledger }: { ledger: DecisionLedger }) {
  const navigate = useNavigate();
  const entries = ledger.entries;
  const largest = Math.max(1, ...entries.map((entry) => Math.abs(entry.value_minor ?? 0)));
  const width = 1000;
  const height = 120;
  const step = width / Math.max(1, entries.length);
  const middle = height / 2;
  const open = (entry: LedgerEntry) => {
    if (entry.fork_run_id) void navigate(`/runs/${entry.fork_run_id}/timeline`);
  };
  return (
    <svg
      role="img"
      aria-label={`Value of each of ${entries.length} decisions against the directive it replaced`}
      viewBox={`0 0 ${width} ${height}`}
      className="w-full"
      style={{ height }}
    >
      <line x1={0} x2={width} y1={middle} y2={middle} stroke="var(--chart-axis)" />
      {entries.map((entry, index) => {
        const value = entry.value_minor ?? 0;
        const size = (Math.abs(value) / largest) * (middle - 6);
        const color =
          entry.status === "unchanged" || entry.status === "at_horizon"
            ? "var(--chart-grid)"
            : entry.status !== "completed"
              ? "var(--text-disabled)"
              : value >= 0
                ? "var(--data-positive)"
                : "var(--data-negative)";
        return (
          <rect
            key={`${entry.agent_id}-${entry.index}`}
            x={index * step + step * 0.15}
            width={step * 0.7}
            y={value >= 0 ? middle - Math.max(size, 1) : middle}
            height={Math.max(size, 1)}
            fill={color}
            className={entry.fork_run_id ? "cursor-pointer" : undefined}
            onClick={() => open(entry)}
          >
            <title>
              {`Decision ${entry.index} at ${formatElapsed(entry.at_ns, "s")}: ${
                entry.status === "unchanged"
                  ? "same directive as before"
                  : entry.status === "completed"
                    ? `${signed(entry.value_minor)} minor over its interval`
                    : entry.status
              }`}
            </title>
          </rect>
        );
      })}
    </svg>
  );
}

function Ledger({ runId }: { runId: string }) {
  const client = useQueryClient();
  const ledger = useQuery(decisionLedgerQuery(runId, true));
  const start = useMutation({
    mutationFn: () => post(`/runs/${runId}/decision-ledger`),
    onSuccess: () =>
      void client.invalidateQueries({ queryKey: ["runs", runId, "decision-ledger"] }),
  });
  const data = ledger.data?.ledger ?? null;
  if (!data) {
    return (
      <div className="flex flex-col gap-2 p-3">
        <p className="text-sm text-fg-2">
          For each decision that changed the directive, the engine forks this run at that moment,
          puts back the directive it replaced, keeps every later decision, and stops at the next
          decision. The difference in the desk&apos;s equity is what that one decision was worth
          over its own interval, on this path.
        </p>
        <div>
          <Button variant="primary" disabled={start.isPending} onClick={() => start.mutate()}>
            {start.isPending ? "Starting…" : "Run the decision ledger"}
          </Button>
        </div>
        {start.isError ? <ErrorState error={start.error} compact /> : null}
      </div>
    );
  }
  const done = data.entries.filter((entry) => entry.status === "completed").length;
  const changed = data.entries.filter(
    (entry) => entry.status !== "unchanged" && entry.status !== "at_horizon",
  ).length;
  const hold = data.versus_hold;
  return (
    <div className="flex flex-col">
      <div className="p-2">
        <LedgerBars ledger={data} />
      </div>
      <KeyValue
        rows={[
          { label: "Status", value: data.status },
          { label: "Decisions forked", value: `${done} of ${changed} that changed the directive` },
          {
            label: "Desk vs. its own start",
            value: hold ? (
              hold.fork_run_id ? (
                <Link to={`/runs/${hold.fork_run_id}/timeline`} className="num underline">
                  {signed(hold.value_minor)} minor
                </Link>
              ) : (
                hold.status
              )
            ) : (
              "—"
            ),
            title:
              "The run against a fork that keeps the initial directive from the first decision",
          },
        ]}
      />
      <p className="p-3 text-xs text-fg-3">
        ONE SAMPLE PATH. The bars are not additive: a decision&apos;s effect outlasts its interval
        and interacts with later ones, so their sum is not the desk&apos;s edge. The last row
        compares the whole run with a desk that never changed its mind.
      </p>
    </div>
  );
}

function DecisionTable({ decisions }: { decisions: DeskDecisionRow[] }) {
  return (
    <table className="w-full border-collapse text-left text-sm">
      <thead className="sticky top-0 bg-surface-1">
        <tr className="label text-fg-3">
          {["#", "Time", "Directive", "Position", "Source", "Rationale (the model's words)"].map(
            (label) => (
              <th key={label} className="border-b border-line px-3 py-1.5 font-medium">
                {label}
              </th>
            ),
          )}
        </tr>
      </thead>
      <tbody>
        {decisions.map((item) => (
          <tr key={`${item.agent_id}-${item.index}`} className="border-b border-line-subtle">
            <td className="num px-3 py-1 text-fg-3">{item.index}</td>
            <td className="num px-3 py-1 text-fg-2">{formatElapsed(item.at_ns, "s")}</td>
            <td className="px-3 py-1">
              <DirectiveChip directive={item.directive} />
            </td>
            <td className="num px-3 py-1 text-right text-fg-2">{formatInteger(item.position)}</td>
            <td
              className={cx(
                "label px-3 py-1 text-xs",
                item.source === "fallback" ? "text-neg" : "text-fg-3",
              )}
              title={item.error_code ?? undefined}
            >
              {item.source}
              {item.error_code ? ` · ${item.error_code}` : ""}
            </td>
            <td className="px-3 py-1 text-xs text-fg-2 italic">
              {item.directive.rationale || "—"}
            </td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}

export function DeskPage() {
  const run = useRun();
  const navigate = useNavigate();
  const desk = (run.desk ?? null) as DeskInfo | null;
  const decisions = useQuery(decisionsQuery(run.id, desk !== null));
  const agentId = desk?.agents[0] ?? 1;
  const market = useQuery(marketSeriesQuery(run.id));
  const equity = useQuery(agentSeriesQuery(run.id, agentId));
  const [xRange, setXRange] = useState<XRange | null>(null);
  const repeat = useMutation({
    mutationFn: () => post<ExperimentSummary>(`/runs/${run.id}/repeat`, { seeds: 20 }),
    onSuccess: (experiment) => void navigate(`/experiments/${experiment.id}`),
  });
  const rows = useMemo(() => decisions.data?.decisions ?? [], [decisions.data]);
  // Only the decisions that changed what the desk does: a repeated directive is no event.
  const events = useMemo(
    () =>
      rows
        .filter(
          (item, index) =>
            index === 0 || !sameDirective(rows[index - 1]?.directive, item.directive),
        )
        .map((item) => ({ ns: item.at_ns, label: item.directive.mode })),
    [rows],
  );
  const price = useMemo(() => {
    if (!market.data) return null;
    const series: SeriesSpec[] = [
      {
        key: "mid",
        label: "Mid",
        color: "--chart-price",
        values: market.data.columns.mid_twice_ticks.map((value) =>
          value === null ? null : value / 2,
        ),
        scale: "price",
      },
    ];
    return { x: market.data.columns.timestamp_ns as number[], series };
  }, [market.data]);
  const money = useMemo(() => {
    if (!equity.data) return null;
    const series: SeriesSpec[] = [
      {
        key: "equity",
        label: "Desk equity",
        color: "--chart-inventory-aware",
        values: equity.data.columns.equity_minor,
        width: 1.5,
      },
      {
        key: "position",
        label: "Position",
        color: "--chart-fundamental",
        values: equity.data.columns.position,
        scale: "position",
      },
    ];
    return { x: equity.data.columns.timestamp_ns as number[], series };
  }, [equity.data]);

  if (!desk) {
    return (
      <div className="h-full bg-surface-0">
        <EmptyState title="No AI desk in this run">
          Start one from the AI Desk screen, or fork this run and switch an agent to the desk.
        </EmptyState>
      </div>
    );
  }
  const usage = desk.usage;
  return (
    <div className="grid min-h-full grid-cols-1 gap-px bg-line-subtle xl:grid-cols-[minmax(0,1fr)_minmax(0,26rem)]">
      <div className="flex min-w-0 flex-col gap-px">
        <InstrumentPanel
          index="01"
          title="Market and decisions"
          subtitle="each rule is a decision that changed the directive"
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
                syncKey={`desk-${run.id}`}
                events={events}
                eventLabels
                showXAxis={false}
                label="Midpoint with the AI desk's decisions"
                description={`Recorded midpoint with ${rows.length} desk decisions marked.`}
              />
            ) : (
              <LoadingState label="Loading market" />
            )}
          </div>
        </InstrumentPanel>
        <InstrumentPanel index="02" title={`Agent ${agentId} equity and position`} unit="minor">
          <div className="relative h-40">
            {money ? (
              <TimeSeriesChart
                x={money.x}
                series={money.series}
                scales={{
                  ...MONEY,
                  position: { format: axisCompact, zero: true, minSpan: 4, side: "right" },
                }}
                xRange={xRange}
                onXRange={setXRange}
                syncKey={`desk-${run.id}`}
                events={events}
                label="The desk's equity and position"
                description="From the run's own accounting, in minor currency units and shares."
              />
            ) : (
              <LoadingState label="Loading the desk's account" />
            )}
          </div>
        </InstrumentPanel>
        <InstrumentPanel
          index="03"
          title="Decisions"
          subtitle="recorded in the trace; replay never asks the model again"
          bodyClassName="overflow-auto"
        >
          {decisions.isError ? (
            <ErrorState error={decisions.error} compact />
          ) : (
            <DecisionTable decisions={rows} />
          )}
        </InstrumentPanel>
      </div>
      <div className="flex min-w-0 flex-col gap-px">
        <InstrumentPanel index="04" title="AI desk" subtitle={desk.label ?? "synthetic market"}>
          <div className="flex flex-col gap-1 border-l-4 border-l-accent bg-surface-1 p-3">
            <div className="label text-sm text-fg">
              {desk.source === "fixture" ? "FIXTURE MODEL" : desk.model}
            </div>
            <p className="text-xs text-fg-3">
              A statement about one model, one mandate, and one synthetic market. Not evidence that
              the model can trade, or about any real instrument.
            </p>
          </div>
          <KeyValue
            rows={[
              { label: "Agent", value: agentId },
              { label: "Decisions", value: formatInteger(rows.length) },
              { label: "Source", value: desk.source },
              { label: "Model calls", value: usage ? formatInteger(usage.calls) : "—" },
              { label: "Failed calls", value: usage ? formatInteger(usage.failures) : "—" },
              {
                label: "Tokens",
                value: usage
                  ? `${formatInteger(usage.input_tokens)} in · ${formatInteger(usage.output_tokens)} out`
                  : "—",
              },
              { label: "Cost", value: usage?.cost_usd ? `${usage.cost_usd} USD` : "—" },
            ]}
          />
          {desk.mandate ? (
            <div className="border-t border-line-subtle p-3">
              <div className="label mb-1 text-fg-3">Mandate</div>
              <p className="text-sm whitespace-pre-wrap text-fg-2">{desk.mandate}</p>
            </div>
          ) : null}
        </InstrumentPanel>
        <InstrumentPanel index="05" title="Decision ledger" subtitle="every bar opens its fork">
          <Ledger runId={run.id} />
        </InstrumentPanel>
        <InstrumentPanel index="06" title="Across seeds">
          <div className="flex flex-col gap-2 p-3">
            <p className="text-sm text-fg-2">
              The desk against the baseline maker it replaced, as agent {agentId}, on 20 paired
              seeds. That is the only claim about the desk beyond one path.
              {desk.source === "model"
                ? ` It asks the model about ${formatInteger(20 * (desk.decisions ?? 0))} times.`
                : ""}
            </p>
            <div>
              <Button disabled={repeat.isPending} onClick={() => repeat.mutate()}>
                {repeat.isPending ? "Starting…" : "Repeat over 20 seeds"}
              </Button>
            </div>
            {repeat.isError ? <ErrorState error={repeat.error} compact /> : null}
          </div>
        </InstrumentPanel>
      </div>
    </div>
  );
}
