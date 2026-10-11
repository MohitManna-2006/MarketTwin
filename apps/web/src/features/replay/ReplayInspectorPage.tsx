import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { useMemo, useState } from "react";

import { DataTable } from "@/components/tables/DataTable";
import { Button } from "@/components/ui/Button";
import { Digest } from "@/components/ui/Digest";
import { InstrumentPanel } from "@/components/ui/InstrumentPanel";
import { IntegerFilter } from "@/components/ui/IntegerFilter";
import { SegmentedControl } from "@/components/ui/SegmentedControl";
import { EmptyState, ErrorState, LoadingState } from "@/components/ui/States";
import { REPLAY_STATUS, StatusIndicator } from "@/components/ui/StatusIndicator";
import { EVENT_COLUMNS, kindLabel } from "@/features/market/EventStream";
import { useRun } from "@/features/runs/RunLayout";
import { usePagedRows } from "@/hooks/usePagedRows";
import { ApiError, get, post } from "@/lib/api/client";
import {
  eventParams,
  eventsAtQuery,
  eventsQuery,
  healthQuery,
  type EventFilter,
} from "@/lib/api/queries";
import type { EventPage, EventRow, ReplayView } from "@/lib/api/types";
import { cx } from "@/lib/cx";
import { formatInteger, formatProcessing, formatWallClock } from "@/lib/formatting";

import { DivergenceReport } from "./DivergenceReport";
import { ReplayDigestTable } from "./ReplayDigestTable";
import { replayCode } from "./replayCodes";

const PAGE = 200;

function Records({ runId, replay }: { runId: string; replay: ReplayView }) {
  const mismatchStream = replay.code === "AccountingMismatch" ? "accounting" : "market";
  const mismatch = replay.status === "mismatch" && replay.sequence ? replay.sequence : null;
  const [stream, setStream] = useState<"market" | "accounting">(
    mismatch ? mismatchStream : "market",
  );
  const [channel, setChannel] = useState<"all" | "commands" | "execution">("all");
  const [kind, setKind] = useState("");
  const [agentId, setAgentId] = useState<number | null>(null);
  const [orderId, setOrderId] = useState<number | null>(null);
  const [jump, setJump] = useState<number | null>(mismatch);
  // A selection belongs to the filter it was made under.
  const [selection, setSelection] = useState<{ key: string; row: EventRow } | null>(null);

  const filter = useMemo<EventFilter>(
    () => ({
      stream,
      channel: stream === "market" && channel !== "all" ? channel : undefined,
      kinds: kind ? [kind] : undefined,
      agentId: stream === "market" ? (agentId ?? undefined) : undefined,
      orderId: stream === "market" ? (orderId ?? undefined) : undefined,
    }),
    [stream, channel, kind, agentId, orderId],
  );
  const filterKey = JSON.stringify([runId, filter]);
  const kinds = useQuery({ ...eventsQuery(runId, { stream }, 0, 1), select: (page) => page.kinds });
  const rows = usePagedRows<EventRow>(filterKey, PAGE, async (offset, limit) => {
    const page = await get<EventPage>(`/runs/${runId}/events`, {
      ...eventParams(filter),
      offset,
      limit,
    });
    return { total: page.total, rows: page.rows };
  });
  const located = useQuery({
    ...eventsAtQuery(runId, filter, { sequence: jump ?? 0 }, 1),
    enabled: jump !== null,
  });
  const anchor = jump !== null ? (located.data?.anchor ?? null) : null;
  const selected = selection?.key === filterKey ? selection.row : null;
  const setSelected = (row: EventRow | null) => setSelection(row ? { key: filterKey, row } : null);

  const columns = useMemo(
    () =>
      stream === "market"
        ? EVENT_COLUMNS
        : EVENT_COLUMNS.filter((column) => !["agent", "order", "state"].includes(column.id)),
    [stream],
  );
  const mismatchId = mismatch && stream === mismatchStream ? mismatch : null;

  return (
    <div className="flex h-full flex-col">
      <div className="flex h-7 shrink-0 items-center gap-2 border-b border-line-subtle px-2">
        <SegmentedControl
          label="Stream"
          value={stream}
          onChange={(value) => {
            setStream(value);
            setKind("");
            setJump(null);
          }}
          options={[
            {
              value: "market",
              label: "Commands and trace",
              title: "commands.jsonl and trace.jsonl, in trace-sequence order",
            },
            {
              value: "accounting",
              label: "Accounting",
              title: "accounting.jsonl, which has its own sequence",
            },
          ]}
        />
        {stream === "market" ? (
          <SegmentedControl
            label="Channel"
            value={channel}
            onChange={setChannel}
            options={[
              { value: "all", label: "All" },
              { value: "commands", label: "Commands" },
              { value: "execution", label: "Outcomes" },
            ]}
          />
        ) : null}
        <label className="flex h-5 items-center border border-line">
          <span className="sr-only">Record kind</span>
          <select
            value={kind}
            onChange={(event) => setKind(event.target.value)}
            className="h-full appearance-none bg-transparent px-1.5 text-xs text-fg-2 outline-none"
          >
            <option value="">All kinds</option>
            {(kinds.data ?? []).map((name) => (
              <option key={name} value={name}>
                {kindLabel(name)}
              </option>
            ))}
          </select>
        </label>
        {stream === "market" ? (
          <>
            <IntegerFilter label="Agent ID" value={agentId} onChange={setAgentId} width={84} />
            <IntegerFilter label="Order ID" value={orderId} onChange={setOrderId} width={128} />
          </>
        ) : null}
        <IntegerFilter
          key={`jump-${jump ?? "none"}`}
          label="Go to sequence"
          value={jump}
          onChange={setJump}
          width={128}
        />
        <span className="flex-1" />
        <span className="num text-xs whitespace-nowrap text-fg-3">
          {rows.isPending ? "…" : formatInteger(rows.rowCount)} records
        </span>
      </div>
      <div className="grid min-h-0 flex-1 grid-cols-[minmax(0,1fr)_320px] divide-x divide-line-subtle">
        <div className="min-h-0">
          {rows.error ? (
            <ErrorState error={rows.error} compact onRetry={rows.retry} />
          ) : rows.isPending ? (
            <LoadingState rows={10} label="Loading records" />
          ) : (
            <DataTable
              label="Recorded events"
              columns={columns}
              rowCount={rows.rowCount}
              getRow={rows.getRow}
              onRangeChange={rows.onRangeChange}
              getRowId={(row) => `${row.channel}-${row.sequence}`}
              selectedId={selected ? `${selected.channel}-${selected.sequence}` : null}
              scrollToIndex={anchor}
              onSelect={setSelected}
              rowClassName={(row) =>
                mismatchId !== null && row.sequence === mismatchId
                  ? "shadow-[inset_3px_0_0_var(--data-negative)] bg-neg-wash"
                  : undefined
              }
              empty={
                <EmptyState title="No records match">
                  Nothing in this stream matches the filters.
                </EmptyState>
              }
            />
          )}
        </div>
        <div className="flex min-h-0 flex-col">
          {selected ? (
            <>
              <div className="flex h-6 shrink-0 items-center gap-2 border-b border-line-subtle bg-surface-1 px-3">
                <span className="label text-fg-2">{kindLabel(selected.kind)}</span>
                <span className="num text-xs text-fg-3">
                  seq {formatInteger(selected.sequence)}
                </span>
                <span className="flex-1" />
                <span className="label text-fg-3">{selected.channel}</span>
              </div>
              <pre
                tabIndex={0}
                className="num min-h-0 flex-1 overflow-auto px-3 py-2 text-xs leading-relaxed whitespace-pre-wrap break-all text-fg-2"
              >
                {JSON.stringify(selected.payload, null, 2)}
              </pre>
              <div className="flex shrink-0 flex-col gap-0.5 border-t border-line-subtle px-3 py-1.5">
                <span className="label text-fg-3">Payload digest</span>
                <Digest value={selected.payload_digest} head={16} tail={8} label="payload digest" />
                <span className="text-2xs text-fg-3">
                  caused by sequence{" "}
                  <span className="num">
                    {selected.causation_id === null
                      ? "none (a command)"
                      : formatInteger(selected.causation_id)}
                  </span>
                </span>
              </div>
            </>
          ) : (
            <EmptyState title="No record selected">
              Select a record to read its payload and digest, exactly as stored.
            </EmptyState>
          )}
        </div>
      </div>
    </div>
  );
}

export function ReplayInspectorPage() {
  const run = useRun();
  const client = useQueryClient();
  const health = useQuery(healthQuery());
  const replay = run.replay;
  const verify = useMutation({
    mutationFn: () => post<ReplayView>(`/runs/${run.id}/replay`),
    onSuccess: () => {
      void client.invalidateQueries({ queryKey: ["runs", run.id] });
      void client.invalidateQueries({ queryKey: ["runs", "list"] });
    },
  });
  const code = replayCode(replay.code);
  const running = replay.status === "running" || verify.isPending;
  const divergent = replay.status === "mismatch" || replay.status === "error";

  const headline: Record<ReplayView["status"], string> = {
    not_checked: "This run has not been verified",
    running: "Regenerating the run and comparing every record",
    verified: "The recording reproduces exactly",
    mismatch: "The recording does not reproduce",
    unsupported_version: "Recorded by a different engine version",
    error: "Verification could not be completed",
  };

  return (
    <div
      className={cx(
        "hairline-grid grid grid-cols-1 lg:h-full lg:grid-cols-[minmax(0,1fr)_400px] 2xl:grid-cols-[minmax(0,1fr)_440px]",
        divergent
          ? "lg:grid-rows-[auto_minmax(0,1.1fr)_minmax(0,1fr)]"
          : "lg:grid-rows-[auto_minmax(0,1fr)]",
      )}
    >
      <InstrumentPanel
        index="01"
        title="Replay status"
        actions={
          <Button size="sm" variant="primary" disabled={running} onClick={() => verify.mutate()}>
            {running
              ? "Verifying"
              : replay.status === "not_checked"
                ? "Verify replay"
                : "Verify again"}
          </Button>
        }
      >
        <div className="flex flex-wrap items-start gap-x-10 gap-y-2 px-4 py-3">
          <div className="min-w-[280px] flex-1">
            <StatusIndicator spec={REPLAY_STATUS[replay.status]} />
            <p className="mt-1 text-lg leading-tight font-medium text-fg">
              {headline[replay.status]}
            </p>
            <p className="mt-1 max-w-[86ch] text-sm leading-snug text-fg-3">
              {replay.status === "not_checked"
                ? "Verification reruns the simulation from config.json, including every agent decision, and requires each command, output, ledger entry, and sample to match the recording byte for byte. It never modifies the run."
                : replay.status === "running"
                  ? "The engine reports nothing until verification returns. A one-hour run takes about a second."
                  : (code?.meaning ?? replay.message)}
            </p>
            {verify.isError ? (
              <p role="alert" className="mt-1 text-sm text-neg">
                {verify.error instanceof ApiError
                  ? `${verify.error.code}: ${verify.error.message}`
                  : "The research API did not respond."}
              </p>
            ) : null}
          </div>
          <dl className="grid grid-cols-[auto_auto] gap-x-4 gap-y-0.5 text-sm">
            <dt className="label self-center text-fg-3">Engine code</dt>
            <dd className="num text-right text-fg">{replay.code ?? "—"}</dd>
            <dt className="label self-center text-fg-3">Checked</dt>
            <dd className="num text-right text-fg-2">{formatWallClock(replay.checked_at)}</dd>
            <dt className="label self-center text-fg-3">Verification time</dt>
            <dd className="num text-right text-fg-2">{formatProcessing(replay.processing_ms)}</dd>
          </dl>
        </div>
      </InstrumentPanel>

      <InstrumentPanel
        index="02"
        title="Artifact provenance"
        subtitle="what a replay checks"
        className={divergent ? "lg:row-span-3" : "lg:row-span-2"}
      >
        <ReplayDigestTable run={run} replay={replay} engineVersion={health.data?.engine_version} />
      </InstrumentPanel>

      {divergent ? (
        <InstrumentPanel
          index="03"
          title="Divergence report"
          subtitle="first difference, as the engine reported it"
          className="max-lg:h-[560px]"
        >
          <DivergenceReport replay={replay} />
        </InstrumentPanel>
      ) : null}

      <InstrumentPanel
        index={divergent ? "04" : "03"}
        title="Recorded events"
        subtitle="versioned records, as stored"
        className="max-lg:h-[560px]"
      >
        <Records
          key={`${replay.status}-${replay.checked_at ?? ""}`}
          runId={run.id}
          replay={replay}
        />
      </InstrumentPanel>
    </div>
  );
}
