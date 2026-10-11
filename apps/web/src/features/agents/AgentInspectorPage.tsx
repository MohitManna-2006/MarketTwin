import { useQuery } from "@tanstack/react-query";
import { useMemo, useState, type ReactNode } from "react";
import { useNavigate, useParams, useSearchParams } from "react-router";

import {
  TimeSeriesChart,
  type ScaleSpec,
  type SeriesSpec,
  type XRange,
} from "@/components/charts/TimeSeriesChart";
import { DataTable, type DataColumn } from "@/components/tables/DataTable";
import { EventTimeline } from "@/components/timeline/EventTimeline";
import { Button } from "@/components/ui/Button";
import { InstrumentPanel } from "@/components/ui/InstrumentPanel";
import { EmptyState, ErrorState, LoadingState } from "@/components/ui/States";
import { useCursorIndex, useRunCursor, useSampleAt } from "@/features/runs/cursor";
import { useRun } from "@/features/runs/RunLayout";
import { shockAnnotations } from "@/features/runs/shocks";
import {
  POLICY_LABEL,
  POLICY_SHORT,
  policySummary,
  wakeupSummary,
} from "@/features/simulations/policySummary";
import { usePagedRows } from "@/hooks/usePagedRows";
import { usePlaybackClock } from "@/hooks/usePlaybackClock";
import { get } from "@/lib/api/client";
import { agentSeriesQuery } from "@/lib/api/queries";
import type {
  AgentConfig,
  AgentReport,
  AgentSeries,
  AgentView,
  LedgerPage,
  LedgerRow,
  RunDetail,
} from "@/lib/api/types";
import { cx } from "@/lib/cx";
import {
  UNAVAILABLE,
  axisCompact,
  formatDuration,
  formatElapsed,
  formatInteger,
  formatPpmAsPercent,
  formatScaled,
  toneOf,
  type Tone,
} from "@/lib/formatting";
import { indexAtOrBefore } from "@/lib/time/asof";
import { usePlayback } from "@/state/playback";

const TONE: Record<Tone, string> = {
  positive: "text-pos",
  negative: "text-neg",
  neutral: "text-fg",
};
const MONEY: Record<string, ScaleSpec> = { y: { format: axisCompact, zero: true, minSpan: 4 } };
const SHARES: Record<string, ScaleSpec> = { y: { format: axisCompact, zero: true, minSpan: 4 } };

function report(agent: AgentView | undefined): AgentReport | null {
  return (agent?.report as AgentReport | null | undefined) ?? null;
}

function Money({ value, signed = true }: { value: number | null | undefined; signed?: boolean }) {
  return (
    <span
      className={cx(
        "num",
        value === null || value === undefined
          ? "text-fg-3"
          : signed
            ? TONE[toneOf(value)]
            : "text-fg",
      )}
    >
      {formatInteger(value, { signed })}
    </span>
  );
}

function Pane({
  index,
  title,
  unit,
  note,
  children,
}: {
  index: string;
  title: string;
  unit: string;
  note?: ReactNode;
  children: ReactNode;
}) {
  return (
    <div className="flex min-h-0 flex-1 flex-col border-b border-line-subtle last:border-b-0">
      <div className="flex h-5 shrink-0 items-center gap-2 bg-surface-1 px-3">
        <span className="label text-fg-2">
          <span className="num font-medium tracking-normal text-accent-label">{index}</span>
          <span className="text-fg-3"> / </span>
          {title}
        </span>
        <span className="label text-fg-3">{unit}</span>
        <span className="flex-1" />
        {note ? <span className="flex items-center gap-3 text-2xs text-fg-3">{note}</span> : null}
      </div>
      <div className="relative min-h-0 flex-1">{children}</div>
    </div>
  );
}

function Key({ color, label, dashed }: { color: string; label: string; dashed?: boolean }) {
  return (
    <span className="flex items-center gap-1.5">
      <span
        aria-hidden
        className="inline-block h-0 w-3 border-t"
        style={{
          borderColor: color,
          borderTopStyle: dashed ? "dashed" : "solid",
          borderTopWidth: 2,
        }}
      />
      <span className="label">{label}</span>
    </span>
  );
}

function columnsOf(series: AgentSeries | undefined, name: keyof AgentSeries["columns"]) {
  return series?.columns[name] ?? [];
}

const LEDGER_COLUMNS: DataColumn<LedgerRow>[] = [
  {
    id: "time",
    header: "Elapsed",
    width: "160px",
    cell: (row) => <span className="num text-fg-2">{formatElapsed(row.timestamp_ns, "ns")}</span>,
  },
  {
    id: "trade",
    header: "Trade",
    title: "Trade ID, in exchange order",
    align: "right",
    width: "60px",
    cell: (row) => <span className="num text-fg-3">{formatInteger(row.trade_id)}</span>,
  },
  {
    id: "side",
    header: "Side",
    width: "44px",
    cell: (row) => (
      <span className={cx("label", row.side === "Buy" ? "text-pos" : "text-neg")}>{row.side}</span>
    ),
  },
  {
    id: "role",
    header: "Role",
    title: "Whether this agent's order was resting (maker) or aggressing (taker)",
    width: "52px",
    cell: (row) => <span className="text-fg-3">{row.role ?? "—"}</span>,
  },
  {
    id: "quantity",
    header: "Qty",
    title: "Shares",
    align: "right",
    width: "50px",
    cell: (row) => <span className="num text-fg">{formatInteger(row.quantity)}</span>,
  },
  {
    id: "price",
    header: "Price",
    title: "Execution price, ticks",
    align: "right",
    width: "62px",
    cell: (row) => <span className="num text-fg">{formatInteger(row.price_ticks)}</span>,
  },
  {
    id: "notional",
    header: "Notional",
    unit: "minor",
    align: "right",
    width: "minmax(92px, 1fr)",
    cell: (row) => <span className="num text-fg-2">{formatInteger(row.notional_minor)}</span>,
  },
  {
    id: "fee",
    header: "Fee",
    title: "Fee paid on this fill; negative is a rebate",
    align: "right",
    width: "52px",
    cell: (row) => (
      <span className="num text-fg-2">
        {formatInteger(row.fee_minor, { signed: row.fee_minor !== 0 })}
      </span>
    ),
  },
  {
    id: "position",
    header: "Position",
    unit: "after",
    title: "Signed position after this trade, shares",
    align: "right",
    width: "minmax(96px, 1fr)",
    cell: (row) => <Money value={row.position} />,
  },
  {
    id: "cash",
    header: "Cash",
    unit: "after",
    title: "Cash after this trade, minor units",
    align: "right",
    width: "minmax(104px, 1fr)",
    cell: (row) => <Money value={row.cash_minor} />,
  },
  {
    id: "realized",
    header: "Realized",
    unit: "cum.",
    title: "Realized P&L after this trade, gross of fees, on an average-cost basis",
    align: "right",
    width: "minmax(104px, 1fr)",
    cell: (row) => <Money value={row.realized_pnl_minor} />,
  },
  {
    id: "counterparty",
    header: "With",
    title: "Counterparty agent",
    align: "right",
    width: "48px",
    cell: (row) => <span className="num text-fg-3">{row.counterparty_agent_id}</span>,
  },
];

function Ledger({
  run,
  agentId,
  times,
}: {
  run: RunDetail;
  agentId: number;
  times: readonly number[];
}) {
  const rows = usePagedRows<LedgerRow>(`${run.id}:${agentId}`, 200, async (offset, limit) => {
    const page = await get<LedgerPage>(`/runs/${run.id}/agents/${agentId}/ledger`, {
      offset,
      limit,
    });
    return { total: page.total, rows: page.rows };
  });
  const [selected, setSelected] = useState<string | null>(null);
  if (rows.error) return <ErrorState error={rows.error} compact onRetry={rows.retry} />;
  if (rows.isPending) return <LoadingState rows={6} label="Loading ledger" />;
  return (
    <DataTable
      label={`Ledger entries of agent ${agentId}`}
      columns={LEDGER_COLUMNS}
      rowCount={rows.rowCount}
      getRow={rows.getRow}
      onRangeChange={rows.onRangeChange}
      getRowId={(row) => String(row.trade_id)}
      selectedId={selected}
      onSelect={(row) => {
        setSelected(String(row.trade_id));
        usePlayback.getState().pause();
        usePlayback.getState().seek(Math.max(0, indexAtOrBefore(times, row.timestamp_ns)));
      }}
      empty={
        <EmptyState title="No trades">
          Agent {agentId} was party to no execution in this run, so the ledger has no entry for it.
          Its account stayed at its opening balance.
        </EmptyState>
      }
    />
  );
}

function AsOf({
  run,
  agent,
  compare,
}: {
  run: RunDetail;
  agent: AgentView;
  compare: AgentView | null;
}) {
  const index = useCursorIndex();
  const sample = useSampleAt(run.id, index, run.agents.length <= 64 ? "all" : String(agent.id));
  const mine = sample.agents[String(agent.id)];
  const other = compare ? sample.agents[String(compare.id)] : undefined;
  const rows: [string, string, number | null | undefined, number | null | undefined][] = [
    ["Position", "shares", mine?.position, other?.position],
    ["Cash", "minor", mine?.cash_minor, other?.cash_minor],
    ["Equity", "minor", mine?.equity_minor, other?.equity_minor],
    ["Realized P&L", "minor", mine?.realized_pnl_minor, other?.realized_pnl_minor],
    ["Unrealized P&L", "minor", mine?.unrealized_pnl_minor, other?.unrealized_pnl_minor],
  ];
  return (
    <MetricTable
      caption={`As of cursor · ${formatElapsed(sample.timestampNs, "ms")}`}
      agent={agent}
      compare={compare}
      rows={rows.map(([label, unit, first, second]) => ({
        label,
        unit,
        first: <Money value={first} />,
        second: <Money value={second} />,
      }))}
    />
  );
}

interface MetricRow {
  label: string;
  unit?: string;
  first: ReactNode;
  second?: ReactNode;
  title?: string;
}

function MetricTable({
  caption,
  agent,
  compare,
  rows,
}: {
  caption: string;
  agent: AgentView;
  compare: AgentView | null;
  rows: MetricRow[];
}) {
  return (
    <table className="w-full border-collapse text-sm">
      <caption className="label border-b border-line bg-surface-1 px-3 py-1.5 text-left text-fg-2">
        {caption}
      </caption>
      {compare ? (
        <thead>
          <tr className="border-b border-line-subtle">
            <th scope="col" className="sr-only">
              Metric
            </th>
            <th scope="col" className="label py-1 pr-3 text-right text-accent">
              Agent {agent.id}
            </th>
            <th scope="col" className="label py-1 pr-3 text-right text-info">
              Agent {compare.id}
            </th>
          </tr>
        </thead>
      ) : null}
      <tbody>
        {rows.map((row) => (
          <tr key={row.label} className="border-b border-line-subtle" title={row.title}>
            <th
              scope="row"
              className="label py-[3px] pl-3 text-left font-semibold whitespace-nowrap text-fg-3"
            >
              {row.label}
              {row.unit ? (
                <span className="font-normal tracking-normal text-fg-3 normal-case">
                  {" "}
                  {row.unit}
                </span>
              ) : null}
            </th>
            <td className="py-[3px] pr-3 text-right">{row.first}</td>
            {compare ? <td className="py-[3px] pr-3 text-right">{row.second}</td> : null}
          </tr>
        ))}
      </tbody>
    </table>
  );
}

function plain(
  value: number | null | undefined,
  format: (value: number | null | undefined) => string = formatInteger,
) {
  const text = format(value);
  return <span className={cx("num", text === UNAVAILABLE ? "text-fg-3" : "text-fg")}>{text}</span>;
}

function Summary({ agent, compare }: { agent: AgentView; compare: AgentView | null }) {
  const first = report(agent);
  const second = report(compare ?? undefined);
  const pair = (
    label: string,
    unit: string,
    render: (item: AgentReport | null) => ReactNode,
    title?: string,
  ): MetricRow => ({ label, unit, first: render(first), second: render(second), title });
  const share = (value: number | null | undefined) => formatPpmAsPercent(value);
  return (
    <MetricTable
      caption="End of run · from metrics.json"
      agent={agent}
      compare={compare}
      rows={[
        pair(
          "Total P&L",
          "minor",
          (item) => <Money value={item?.total_pnl_minor} />,
          "Equity minus opening equity: realized − fees + unrealized",
        ),
        pair(
          "Realized P&L",
          "minor",
          (item) => <Money value={item?.realized_pnl_minor} />,
          "Gross of fees, average-cost basis",
        ),
        pair(
          "Unrealized P&L",
          "minor",
          (item) => <Money value={item?.unrealized_pnl_minor} />,
          "Final position at the final mark, minus cost basis",
        ),
        pair(
          "Fees paid",
          "minor",
          (item) => plain(item?.fees_paid_minor),
          "Negative when rebates exceed fees",
        ),
        pair(
          "Max drawdown",
          "minor",
          (item) => plain(item?.max_drawdown_minor),
          "Largest fall of equity from its running peak, over the start, every sample, and the end",
        ),
        pair("Peak equity", "minor", (item) => <Money value={item?.peak_equity_minor} />),
        pair("Final position", "shares", (item) => <Money value={item?.position} />),
        pair(
          "Max |position|",
          "shares",
          (item) => plain(item?.max_abs_position),
          "Exact: updated on every fill",
        ),
        pair(
          "Mean |position|",
          "shares",
          (item) =>
            plain(item?.mean_abs_position_millishares, (value) =>
              formatScaled(value, 3, { digits: 1 }),
            ),
          "Over the sampling schedule",
        ),
        pair(
          "Maker spread capture",
          "minor",
          (item) => <Money value={item?.maker_spread_capture_minor} />,
          "Edge against the midpoint each aggressor met, as maker",
        ),
        pair(
          "Taker spread cost",
          "minor",
          (item) => plain(item?.taker_spread_cost_minor),
          "The same edge, paid as taker",
        ),
        pair("Fills", "maker / taker", (item) => (
          <span className="num text-fg">
            {item
              ? `${formatInteger(item.maker_fills)} / ${formatInteger(item.taker_fills)}`
              : UNAVAILABLE}
          </span>
        )),
        pair(
          "Fill rate",
          "",
          (item) => plain(item?.fill_rate_ppm, share),
          "Shares filled over shares of accepted orders",
        ),
        pair("Orders rejected", "", (item) => plain(item?.orders_rejected)),
        pair(
          "Cash refusals",
          "",
          (item) => plain(item?.cash_refusals),
          "Buy intents dropped by the credit limit",
        ),
      ]}
    />
  );
}

function Configuration({ agent }: { agent: AgentView }) {
  const config = agent.config as AgentConfig | null;
  if (!config) {
    return (
      <div className="border-b border-line-subtle px-3 py-2">
        <p className="label text-fg-2">Scripted participant</p>
        <p className="mt-1 text-xs leading-snug text-fg-3">
          This agent has no policy. Its orders came from scripted commands in the config, so there
          are no strategy parameters, latencies, or limits to show.
        </p>
      </div>
    );
  }
  const rows: [string, string][] = [
    ["Policy", POLICY_LABEL[config.policy.kind] ?? config.policy.kind],
    ["Parameters", policySummary(config.policy)],
    ["Wakeup", wakeupSummary(config)],
    [
      "Latency",
      `entry ${formatDuration(config.latency.order_entry_ns)} +${formatDuration(config.latency.order_entry_jitter_ns)} · report ${formatDuration(config.latency.report_ns)} · data ${formatDuration(config.latency.market_data_ns)}`,
    ],
    ["Position limit", `±${formatInteger(config.limits.max_position)} shares`],
    [
      "Order limit",
      `${formatInteger(config.limits.max_order_quantity)} shares · ${config.limits.max_open_orders} open`,
    ],
    ["Opening cash", `${formatInteger(agent.initial_cash_minor)} minor`],
    [
      "Credit limit",
      agent.credit_limit_minor === null
        ? "none: cash unconstrained"
        : `${formatInteger(agent.credit_limit_minor)} minor`,
    ],
  ];
  return (
    <div className="text-sm">
      <h3 className="label border-b border-line bg-surface-1 px-3 py-1.5 text-fg-2">
        Configured strategy · agent {agent.id}
      </h3>
      <dl>
        {rows.map(([label, value]) => (
          <div key={label} className="flex gap-3 border-b border-line-subtle px-3 py-[3px]">
            <dt className="label w-[92px] shrink-0 pt-0.5 text-fg-3">{label}</dt>
            <dd className="num min-w-0 flex-1 text-right text-xs leading-snug whitespace-normal text-fg-2">
              {value}
            </dd>
          </div>
        ))}
      </dl>
    </div>
  );
}

export function AgentInspectorPage() {
  const run = useRun();
  const navigate = useNavigate();
  const params = useParams();
  const [search, setSearch] = useSearchParams();
  const times = useRunCursor(run.id);
  usePlaybackClock();
  const [xRange, setXRange] = useState<XRange | null>(null);
  const [lower, setLower] = useState<"drawdown" | "volume">("drawdown");

  const fallback = run.agents.find((item) => item.policy !== null) ?? run.agents[0];
  const agentId =
    params.agentId !== undefined && /^\d+$/.test(params.agentId)
      ? Number(params.agentId)
      : fallback?.id;
  const agent = run.agents.find((item) => item.id === agentId);
  const compareParam = search.get("compare");
  const compare =
    compareParam !== null && Number(compareParam) !== agentId
      ? (run.agents.find((item) => item.id === Number(compareParam)) ?? null)
      : null;

  const series = useQuery({
    ...agentSeriesQuery(run.id, agentId ?? 0),
    enabled: agent !== undefined,
  });
  const other = useQuery({
    ...agentSeriesQuery(run.id, compare?.id ?? 0),
    enabled: compare !== null,
  });
  const { events, bands } = useMemo(() => shockAnnotations(run.shocks), [run.shocks]);

  const chart = useMemo(() => {
    const mine = series.data;
    const theirs = compare ? other.data : undefined;
    const second = (name: keyof AgentSeries["columns"], label: string): SeriesSpec[] =>
      theirs && theirs.columns.timestamp_ns.length === mine?.columns.timestamp_ns.length
        ? [
            {
              key: `b-${name}`,
              label,
              color: "--chart-fundamental",
              values: columnsOf(theirs, name),
              dash: [5, 3],
              width: 1.25,
            },
          ]
        : [];
    return {
      x: (mine?.columns.timestamp_ns ?? []) as number[],
      inventory: [
        ...second("position", `Agent ${compare?.id ?? ""} position`),
        {
          key: "position",
          label: "Position",
          color: "--chart-price",
          values: columnsOf(mine, "position"),
          fill: !theirs,
        },
      ] satisfies SeriesSpec[],
      equity: [
        ...second("equity_minor", `Agent ${compare?.id ?? ""} equity`),
        {
          key: "equity",
          label: "Equity",
          color: "--chart-price",
          values: columnsOf(mine, "equity_minor"),
        },
      ] satisfies SeriesSpec[],
      pnl: [
        {
          key: "unrealized",
          label: "Unrealized",
          color: "--chart-baseline",
          values: columnsOf(mine, "unrealized_pnl_minor"),
          width: 1,
        },
        {
          key: "realized",
          label: "Realized",
          color: "--chart-price",
          values: columnsOf(mine, "realized_pnl_minor"),
        },
      ] satisfies SeriesSpec[],
      drawdown: [
        ...second("drawdown_minor", `Agent ${compare?.id ?? ""} drawdown`),
        {
          key: "drawdown",
          label: "Drawdown",
          color: "--data-negative",
          values: columnsOf(mine, "drawdown_minor"),
          fill: !theirs,
        },
      ] satisfies SeriesSpec[],
      volume: [
        {
          key: "bought",
          label: "Bought",
          color: "--data-positive",
          values: columnsOf(mine, "cum_bought"),
          width: 1.25,
        },
        {
          key: "sold",
          label: "Sold",
          color: "--data-negative",
          values: columnsOf(mine, "cum_sold"),
          width: 1.25,
        },
      ] satisfies SeriesSpec[],
    };
  }, [series.data, other.data, compare]);

  if (!agent) {
    return (
      <div className="h-full bg-surface-0">
        <EmptyState title="No such agent">
          {run.agents.length === 0
            ? "This run registered no agents with the exchange."
            : `This run has no agent ${params.agentId ?? ""}. Choose one from the run's population.`}
        </EmptyState>
      </div>
    );
  }
  if (times.isPending) return <LoadingState label="Loading recorded samples" />;
  if (times.isError) return <ErrorState error={times.error} onRetry={() => void times.refetch()} />;

  const sampleTimes = times.data.timestamp_ns;
  const seek = (ns: number) => {
    usePlayback.getState().pause();
    usePlayback.getState().seek(Math.max(0, indexAtOrBefore(sampleTimes, ns)));
  };
  const common = {
    x: chart.x,
    xRange,
    onXRange: setXRange,
    cursorTimes: sampleTimes,
    onSeek: seek,
    syncKey: `agent-${run.id}`,
    events,
    bands,
  };
  const noSeries = chart.x.length === 0;
  const firstReport = report(agent);
  const legend = compare ? (
    <>
      <Key color="var(--chart-price)" label={`Agent ${agent.id}`} />
      <Key color="var(--chart-fundamental)" label={`Agent ${compare.id}`} dashed />
    </>
  ) : null;

  return (
    <div className="flex flex-col lg:h-full">
      <div className="hairline-grid grid min-h-0 flex-1 grid-cols-1 lg:grid-cols-[208px_minmax(0,1fr)_348px]">
        <InstrumentPanel
          index="01"
          title="Agents"
          subtitle={`${run.agents.length} in run`}
          className="max-lg:max-h-60"
          bodyClassName="overflow-auto"
        >
          <ul>
            {run.agents.map((item) => {
              const itemReport = report(item);
              const active = item.id === agent.id;
              const compared = compare?.id === item.id;
              return (
                <li key={item.id} className="border-b border-line-subtle">
                  <div
                    className={cx(
                      "flex items-stretch",
                      active && "bg-surface-selected shadow-[inset_2px_0_0_var(--accent-primary)]",
                      compared && "bg-info-wash shadow-[inset_2px_0_0_var(--data-neutral)]",
                    )}
                  >
                    <button
                      type="button"
                      aria-current={active ? "true" : undefined}
                      onClick={() =>
                        void navigate({
                          pathname: `/runs/${run.id}/agents/${item.id}`,
                          search: search.toString(),
                        })
                      }
                      className="transition-hover flex min-w-0 flex-1 flex-col px-3 py-1 text-left hover:bg-surface-hover"
                    >
                      <span className="flex items-baseline gap-2">
                        <span className="num text-sm font-medium text-fg">{item.id}</span>
                        <span className="truncate text-sm text-fg-2">
                          {item.policy ? (POLICY_SHORT[item.policy] ?? item.policy) : "scripted"}
                        </span>
                      </span>
                      <span className="flex items-baseline gap-1">
                        <span className="label text-fg-3">P&amp;L</span>
                        <span
                          className={cx("num text-xs", TONE[toneOf(itemReport?.total_pnl_minor)])}
                        >
                          {formatInteger(itemReport?.total_pnl_minor, { signed: true })}
                        </span>
                      </span>
                    </button>
                    {!active ? (
                      <button
                        type="button"
                        aria-pressed={compared}
                        aria-label={
                          compared
                            ? `Stop comparing with agent ${item.id}`
                            : `Compare with agent ${item.id}`
                        }
                        title="Compare with the selected agent. Both traded in this same run."
                        onClick={() =>
                          setSearch(
                            (current) => {
                              const next = new URLSearchParams(current);
                              if (compared) next.delete("compare");
                              else next.set("compare", String(item.id));
                              return next;
                            },
                            { replace: true },
                          )
                        }
                        className={cx(
                          "transition-hover label w-8 shrink-0 border-l border-line-subtle",
                          compared
                            ? "text-info"
                            : "text-fg-3 hover:bg-surface-hover hover:text-fg-2",
                        )}
                      >
                        vs
                      </button>
                    ) : null}
                  </div>
                </li>
              );
            })}
          </ul>
        </InstrumentPanel>

        <div className="hairline-grid grid min-h-0 grid-rows-[minmax(0,1fr)_minmax(120px,30%)] max-lg:h-[960px]">
          <InstrumentPanel
            index="02"
            title={`Agent ${agent.id} over the run`}
            subtitle={
              compare
                ? `compared with agent ${compare.id} · same run, same market history`
                : series.data?.decimated
                  ? `${formatInteger(series.data.index.length)} of ${formatInteger(series.data.sample_count)} samples plotted`
                  : "recorded on the sampling schedule · steps between samples"
            }
            actions={
              <Button
                size="sm"
                variant="ghost"
                disabled={xRange === null}
                onClick={() => setXRange(null)}
              >
                Reset zoom
              </Button>
            }
            bodyClassName="flex flex-col"
          >
            {series.isError ? (
              <ErrorState error={series.error} onRetry={() => void series.refetch()} />
            ) : series.isPending ? (
              <LoadingState label="Loading agent series" />
            ) : noSeries ? (
              <EmptyState title="No time series for this run">
                Accounts are sampled on the run&apos;s sampling schedule and this run recorded no
                samples. The end-of-run figures on the right are still exact.
              </EmptyState>
            ) : (
              <>
                <Pane index="A" title="Inventory" unit="shares, signed" note={legend}>
                  <TimeSeriesChart
                    {...common}
                    series={chart.inventory}
                    scales={SHARES}
                    showXAxis={false}
                    eventLabels
                    label={`Agent ${agent.id} inventory over elapsed simulation time`}
                    description="Recorded signed position at each plotted sample."
                  />
                </Pane>
                <Pane
                  index="B"
                  title="Equity"
                  unit="minor units"
                  note="cash plus position at each sample's mark"
                >
                  <TimeSeriesChart
                    {...common}
                    series={chart.equity}
                    scales={MONEY}
                    showXAxis={false}
                    label={`Agent ${agent.id} equity over elapsed simulation time`}
                    description="Recorded equity at each plotted sample."
                  />
                </Pane>
                <Pane
                  index="C"
                  title="Realized and unrealized P&L"
                  unit="minor units"
                  note={
                    <>
                      <Key color="var(--chart-price)" label="Realized" />
                      <Key color="var(--chart-baseline)" label="Unrealized" />
                    </>
                  }
                >
                  <TimeSeriesChart
                    {...common}
                    series={chart.pnl}
                    scales={MONEY}
                    showXAxis={false}
                    label={`Agent ${agent.id} realized and unrealized profit and loss`}
                    description="Recorded realized and unrealized profit and loss at each plotted sample."
                  />
                </Pane>
                <Pane
                  index="D"
                  title={lower === "drawdown" ? "Drawdown" : "Executed volume"}
                  unit={lower === "drawdown" ? "minor units" : "shares, cumulative"}
                  note={
                    <>
                      <span>
                        {lower === "drawdown"
                          ? `derived: running peak of sampled equity minus equity · native maximum ${formatInteger(firstReport?.max_drawdown_minor)}`
                          : "derived: fills booked before each sample, from the ledger"}
                      </span>
                      <button
                        type="button"
                        className="transition-hover label text-fg-2 underline decoration-line-strong underline-offset-2 hover:text-fg"
                        onClick={() => setLower(lower === "drawdown" ? "volume" : "drawdown")}
                      >
                        Show {lower === "drawdown" ? "executed volume" : "drawdown"}
                      </button>
                    </>
                  }
                >
                  <TimeSeriesChart
                    {...common}
                    series={lower === "drawdown" ? chart.drawdown : chart.volume}
                    scales={lower === "drawdown" ? MONEY : SHARES}
                    label={`Agent ${agent.id} ${lower === "drawdown" ? "drawdown" : "cumulative executed volume"}`}
                    description={
                      lower === "drawdown"
                        ? "Running peak of sampled equity minus equity. The core's maximum drawdown is in the summary table."
                        : "Cumulative shares bought and sold, from ledger entries booked before each sample."
                    }
                  />
                </Pane>
              </>
            )}
          </InstrumentPanel>
          <InstrumentPanel
            index="03"
            title="Ledger"
            subtitle={`agent ${agent.id} · one entry per trade, balances after it`}
          >
            <Ledger run={run} agentId={agent.id} times={sampleTimes} />
          </InstrumentPanel>
        </div>

        <InstrumentPanel
          index="04"
          title={`Agent ${agent.id}`}
          subtitle={agent.policy ? (POLICY_LABEL[agent.policy] ?? agent.policy) : "scripted"}
          bodyClassName="overflow-auto"
        >
          {sampleTimes.length > 0 ? <AsOf run={run} agent={agent} compare={compare} /> : null}
          <Summary agent={agent} compare={compare} />
          {firstReport?.total_pnl_minor === null ? (
            <p className="border-b border-line-subtle px-3 py-2 text-xs leading-snug text-warn">
              This agent ended with an open position and no mark, so it has no valuation: total
              P&amp;L, unrealized P&amp;L, and equity are undefined, not zero.
            </p>
          ) : null}
          <Configuration agent={agent} />
          {compare ? (
            <p className="px-3 py-2 text-xs leading-snug text-fg-3">
              Agents {agent.id} and {compare.id} traded in the same run, so they met the same
              market. To compare two strategies in the same seat, use a paired experiment.
            </p>
          ) : null}
        </InstrumentPanel>
      </div>
      <EventTimeline times={sampleTimes} events={events} bands={bands} xRange={xRange} />
    </div>
  );
}
