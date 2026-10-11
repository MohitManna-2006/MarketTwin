import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { useMemo } from "react";
import { useNavigate } from "react-router";

import { ContextBar } from "@/components/shell/ContextBar";
import { Page } from "@/components/shell/TerminalShell";
import { DataTable, useArrayRows, type DataColumn } from "@/components/tables/DataTable";
import { Button } from "@/components/ui/Button";
import { Digest } from "@/components/ui/Digest";
import { InstrumentPanel } from "@/components/ui/InstrumentPanel";
import { MetricReadout } from "@/components/ui/MetricReadout";
import { EmptyState, ErrorState, LoadingState } from "@/components/ui/States";
import { REPLAY_STATUS, StatusIndicator } from "@/components/ui/StatusIndicator";
import { RUN_COLUMNS } from "@/features/runs/runColumns";
import { post } from "@/lib/api/client";
import { runsQuery } from "@/lib/api/queries";
import type { ReplayView, RunSummary } from "@/lib/api/types";
import { formatInteger } from "@/lib/formatting";

export function ReplayIndexPage() {
  const navigate = useNavigate();
  const client = useQueryClient();
  const runs = useQuery(runsQuery(1000, true));
  const completed = useMemo(
    () => (runs.data?.runs ?? []).filter((run) => run.status === "completed"),
    [runs.data],
  );
  const rows = useArrayRows(completed);
  const verify = useMutation({
    mutationFn: (runId: string) => post<ReplayView>(`/runs/${runId}/replay`),
    onSettled: () => void client.invalidateQueries({ queryKey: ["runs"] }),
  });

  const columns = useMemo<DataColumn<RunSummary>[]>(
    () => [
      RUN_COLUMNS.name,
      RUN_COLUMNS.scenario,
      RUN_COLUMNS.seed,
      {
        id: "engine",
        header: "Engine",
        width: "72px",
        cell: (run) => <span className="num text-fg-2">{run.engine_version}</span>,
      },
      {
        id: "digest",
        header: "Config digest",
        width: "190px",
        cell: (run) => <Digest value={run.config_digest} label="config digest" />,
      },
      {
        id: "replay",
        header: "Replay",
        width: "190px",
        sortValue: (run) => run.replay_status,
        cell: (run) => <StatusIndicator spec={REPLAY_STATUS[run.replay_status]} />,
      },
      {
        id: "action",
        header: "",
        align: "right",
        width: "104px",
        cell: (run) => (
          <Button
            size="sm"
            variant="ghost"
            disabled={run.replay_status === "running"}
            onClick={(event) => {
              event.stopPropagation();
              verify.mutate(run.id);
            }}
          >
            {run.replay_status === "not_checked" ? "Verify" : "Verify again"}
          </Button>
        ),
      },
      RUN_COLUMNS.created,
    ],
    [verify],
  );

  const count = (status: string) => completed.filter((run) => run.replay_status === status).length;

  return (
    <Page
      context={
        <ContextBar crumbs={[{ label: "Replay" }, { label: "Verification of stored runs" }]} />
      }
    >
      <div className="hairline-grid grid h-full grid-rows-[auto_minmax(0,1fr)]">
        <section
          aria-label="Verification summary"
          className="flex flex-wrap items-stretch bg-surface-0"
        >
          {(
            [
              ["Completed runs", formatInteger(completed.length), "can be replayed"],
              ["Verified", formatInteger(count("verified")), "reproduce exactly"],
              ["Mismatch", formatInteger(count("mismatch")), "differ from their recording"],
              ["Not checked", formatInteger(count("not_checked")), "never verified"],
              [
                "Unsupported",
                formatInteger(count("unsupported_version")),
                "another engine version",
              ],
            ] as const
          ).map(([label, value, note]) => (
            <div
              key={label}
              className="min-w-[140px] flex-1 border-r border-line-subtle px-4 py-2.5 last:border-r-0"
            >
              <MetricReadout label={label} value={runs.data ? value : "—"} note={note} size="lg" />
            </div>
          ))}
        </section>
        <InstrumentPanel
          index="01"
          title="Stored runs"
          subtitle="select a run to inspect its recording and its last verification"
        >
          {runs.isPending ? (
            <LoadingState rows={10} label="Loading runs" />
          ) : runs.isError ? (
            <ErrorState error={runs.error} onRetry={() => void runs.refetch()} />
          ) : (
            <DataTable
              label="Runs and their replay status"
              columns={columns}
              {...rows}
              getRowId={(run) => run.id}
              onSelect={(run) => void navigate(`/runs/${run.id}/replay`)}
              onActivate={(run) => void navigate(`/runs/${run.id}/replay`)}
              empty={
                <EmptyState
                  title="No completed run to replay"
                  actions={
                    <Button
                      size="sm"
                      variant="primary"
                      onClick={() => void navigate("/simulations/new")}
                    >
                      New simulation
                    </Button>
                  }
                >
                  Replay regenerates a completed run from its config and seed and compares every
                  record with what was stored. Run a simulation first.
                </EmptyState>
              }
            />
          )}
        </InstrumentPanel>
      </div>
    </Page>
  );
}
