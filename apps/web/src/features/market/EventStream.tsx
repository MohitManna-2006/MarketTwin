import { useQuery } from "@tanstack/react-query";
import { useMemo, useState } from "react";

import { DataTable, type DataColumn } from "@/components/tables/DataTable";
import { IntegerFilter } from "@/components/ui/IntegerFilter";
import { SegmentedControl } from "@/components/ui/SegmentedControl";
import { EmptyState, ErrorState, LoadingState } from "@/components/ui/States";
import { useDebounced } from "@/hooks/useDebounced";
import { usePagedRows } from "@/hooks/usePagedRows";
import { get } from "@/lib/api/client";
import { eventParams, eventsAtQuery, type EventFilter } from "@/lib/api/queries";
import type { EventPage, EventRow } from "@/lib/api/types";
import { cx } from "@/lib/cx";
import { formatElapsed, formatInteger } from "@/lib/formatting";

const PAGE = 200;

const KIND_TONE: Record<string, string> = {
  Trade: "text-accent",
  ScenarioShock: "text-warn",
  OrderArrival: "text-fg-2",
  CancelArrival: "text-fg-3",
  OrderStatus: "text-fg-2",
  CancelResult: "text-fg-3",
  BookSnapshot: "text-fg-3",
  SnapshotRequest: "text-fg-3",
  FundamentalValueUpdate: "text-info",
  RunCompleted: "text-pos",
  LedgerEntry: "text-accent",
  Sample: "text-fg-3",
};

export function kindLabel(kind: string): string {
  return kind.replace(/([a-z])([A-Z])/g, "$1 $2");
}

export const EVENT_COLUMNS: DataColumn<EventRow>[] = [
  {
    id: "time",
    header: "Elapsed",
    width: "160px",
    title: "Elapsed simulation time, to the nanosecond",
    cell: (row) => <span className="num text-fg-2">{formatElapsed(row.timestamp_ns, "ns")}</span>,
  },
  {
    id: "sequence",
    header: "Seq",
    align: "right",
    width: "60px",
    title: "Trace sequence: dispatch order of commands and their outcomes",
    cell: (row) => <span className="num text-fg-3">{formatInteger(row.sequence)}</span>,
  },
  {
    id: "agent",
    header: "Agent",
    align: "right",
    width: "62px",
    cell: (row) => <span className="num text-fg-2">{row.actor_id ?? "—"}</span>,
  },
  {
    id: "kind",
    header: "Event",
    width: "minmax(140px, 1.3fr)",
    cell: (row) => (
      <span className="flex items-baseline gap-1.5">
        <span className={KIND_TONE[row.kind] ?? "text-fg-2"}>{kindLabel(row.kind)}</span>
        {row.side ? (
          <span className={cx("label", row.side === "Buy" ? "text-pos" : "text-neg")}>
            {row.side}
          </span>
        ) : null}
      </span>
    ),
  },
  {
    id: "order",
    header: "Order ID",
    align: "right",
    width: "98px",
    cell: (row) => <span className="num text-fg-3">{row.order_id ?? "—"}</span>,
  },
  {
    id: "price",
    header: "Price",
    title: "Limit price of an order, or execution price of a trade, in ticks",
    align: "right",
    width: "60px",
    cell: (row) => <span className="num text-fg">{formatInteger(row.price_ticks)}</span>,
  },
  {
    id: "quantity",
    header: "Qty",
    title: "Shares",
    align: "right",
    width: "46px",
    cell: (row) => <span className="num text-fg">{formatInteger(row.quantity)}</span>,
  },
  {
    id: "state",
    header: "Status",
    width: "minmax(78px, 0.8fr)",
    cell: (row) => {
      const text = row.reason ?? row.state;
      if (!text) return <span className="text-fg-3">—</span>;
      return <span className={row.reason ? "text-neg" : "text-fg-2"}>{kindLabel(text)}</span>;
    },
  },
];

export interface EventStreamProps {
  runId: string;
  /** Elapsed time of the cursor; the stream follows it to the latest record at or before it. */
  cursorNs: number | null;
  /** Limits the stream to one agent's commands, outcomes, and fills as maker. */
  agentId: number | null;
  onSeekTime: (ns: number) => void;
  kinds: readonly string[];
  toolbar?: (controls: React.ReactNode) => React.ReactNode;
}

/**
 * The run's commands and their outcomes in dispatch order. It follows the time cursor:
 * the highlighted row is the latest record at or before it.
 */
export function EventStream({ runId, cursorNs, agentId, onSeekTime, kinds }: EventStreamProps) {
  const [channel, setChannel] = useState<"all" | "commands" | "execution">("all");
  const [kind, setKind] = useState("");
  const [orderId, setOrderId] = useState<number | null>(null);
  const [follow, setFollow] = useState(true);
  // A selection belongs to the filter it was made under.
  const [selection, setSelection] = useState<{ key: string; row: EventRow } | null>(null);

  const filter = useMemo<EventFilter>(
    () => ({
      stream: "market",
      channel: channel === "all" ? undefined : channel,
      kinds: kind ? [kind] : undefined,
      agentId: agentId ?? undefined,
      orderId: orderId ?? undefined,
    }),
    [channel, kind, agentId, orderId],
  );
  const filterKey = JSON.stringify([runId, filter]);
  const rows = usePagedRows<EventRow>(filterKey, PAGE, async (offset, limit) => {
    const page = await get<EventPage>(`/runs/${runId}/events`, {
      ...eventParams(filter),
      offset,
      limit,
    });
    return { total: page.total, rows: page.rows };
  });

  const settled = useDebounced(cursorNs, 120);
  const anchorQuery = useQuery({
    ...eventsAtQuery(runId, filter, { ns: settled ?? 0 }, 1),
    enabled: follow && settled !== null,
    placeholderData: (previous) => previous,
  });
  const anchor = anchorQuery.data?.anchor ?? null;
  const anchorRow = anchorQuery.data?.rows[0];
  const selected = selection?.key === filterKey ? selection.row : null;
  const setSelected = (row: EventRow | null) => setSelection(row ? { key: filterKey, row } : null);

  const highlighted = follow && anchorRow ? `${anchorRow.channel}-${anchorRow.sequence}` : null;
  const selectedId = selected ? `${selected.channel}-${selected.sequence}` : highlighted;

  return (
    <div className="flex h-full flex-col">
      <div className="flex h-7 shrink-0 items-center gap-2 border-b border-line-subtle bg-surface-0 px-2">
        <SegmentedControl
          label="Channel"
          value={channel}
          onChange={setChannel}
          options={[
            { value: "all", label: "All" },
            { value: "commands", label: "Commands", title: "Inputs to the exchange" },
            { value: "execution", label: "Outcomes", title: "What the exchange returned" },
          ]}
        />
        <label className="flex h-5 items-center border border-line">
          <span className="sr-only">Event kind</span>
          <select
            value={kind}
            onChange={(event) => setKind(event.target.value)}
            className="h-full appearance-none bg-transparent px-1.5 text-xs text-fg-2 outline-none"
          >
            <option value="">All events</option>
            {kinds.map((name) => (
              <option key={name} value={name}>
                {kindLabel(name)}
              </option>
            ))}
          </select>
        </label>
        <IntegerFilter label="Order ID" value={orderId} onChange={setOrderId} width={128} />
        {agentId !== null ? (
          <span className="label shrink-0 whitespace-nowrap text-accent">Agent {agentId}</span>
        ) : null}
        <span className="flex-1" />
        <span className="num shrink-0 text-xs whitespace-nowrap text-fg-3">
          {rows.isPending ? "…" : formatInteger(rows.rowCount)} records
        </span>
        <button
          type="button"
          aria-pressed={follow}
          onClick={() => {
            setFollow((value) => !value);
            setSelected(null);
          }}
          title="Keep the latest record at or before the time cursor in view"
          className={cx(
            "transition-hover label h-5 shrink-0 border px-1.5 whitespace-nowrap",
            follow
              ? "border-accent-muted bg-accent-wash text-accent"
              : "border-line text-fg-3 hover:text-fg-2",
          )}
        >
          Follow cursor
        </button>
      </div>
      <div className="min-h-0 flex-1">
        {rows.error ? (
          <ErrorState error={rows.error} compact onRetry={rows.retry} />
        ) : rows.isPending ? (
          <LoadingState rows={8} label="Loading records" />
        ) : (
          <DataTable
            label="Execution stream"
            columns={EVENT_COLUMNS}
            rowCount={rows.rowCount}
            getRow={rows.getRow}
            onRangeChange={rows.onRangeChange}
            getRowId={(row) => `${row.channel}-${row.sequence}`}
            selectedId={selectedId}
            scrollToIndex={follow && !selected ? anchor : null}
            onSelect={(row) => {
              setSelected(row);
              onSeekTime(row.timestamp_ns);
            }}
            empty={
              <EmptyState title="No records match">
                {agentId !== null || kind || orderId !== null || channel !== "all"
                  ? "Nothing in this run matches the current filters."
                  : "This run dispatched no commands."}
              </EmptyState>
            }
          />
        )}
      </div>
    </div>
  );
}
