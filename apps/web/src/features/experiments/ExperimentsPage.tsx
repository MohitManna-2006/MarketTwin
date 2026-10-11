import { useQuery } from "@tanstack/react-query";
import { useNavigate } from "react-router";

import { ContextBar } from "@/components/shell/ContextBar";
import { Page } from "@/components/shell/TerminalShell";
import { DataTable, useArrayRows, type DataColumn } from "@/components/tables/DataTable";
import { Button } from "@/components/ui/Button";
import { InstrumentPanel } from "@/components/ui/InstrumentPanel";
import { EmptyState, ErrorState, LoadingState } from "@/components/ui/States";
import { EXPERIMENT_STATUS, StatusIndicator } from "@/components/ui/StatusIndicator";
import { experimentsQuery } from "@/lib/api/queries";
import type { ExperimentSummary } from "@/lib/api/types";
import { formatInteger, formatProcessing, formatWallClock } from "@/lib/formatting";

import { DifferenceCell } from "./DifferenceCell";

const COLUMNS: DataColumn<ExperimentSummary>[] = [
  {
    id: "experiment",
    header: "Experiment",
    width: "minmax(200px, 1.3fr)",
    cell: (item) => (
      <span className="flex min-w-0 items-baseline gap-2">
        <span className="truncate font-medium text-fg">{item.name}</span>
        <span className="num shrink-0 text-xs text-fg-3">{item.id}</span>
      </span>
    ),
  },
  {
    id: "scenario",
    header: "Scenario",
    width: "minmax(110px, 0.8fr)",
    cell: (item) => (
      <span className="text-fg-2">{item.scenario_key?.replaceAll("_", " ") ?? "custom"}</span>
    ),
  },
  {
    id: "comparison",
    header: "Comparison",
    title: "Candidate minus baseline, paired by seed",
    width: "minmax(220px, 1.4fr)",
    cell: (item) =>
      item.comparison ? (
        <span className="text-fg-2">
          {String(item.comparison.candidate)}
          <span className="text-fg-3"> minus </span>
          {String(item.comparison.baseline)}
        </span>
      ) : (
        <span className="text-fg-3">{item.variants.join(", ")} · no comparison</span>
      ),
  },
  {
    id: "seeds",
    header: "Seeds",
    align: "right",
    width: "64px",
    cell: (item) => <span className="num text-fg-2">{formatInteger(item.seed_count)}</span>,
  },
  {
    id: "runs",
    header: "Runs",
    title: "Completed of total; failed runs are counted, never dropped",
    align: "right",
    width: "140px",
    cell: (item) => (
      <span className="num text-fg-2">
        {formatInteger(item.completed_runs)}
        <span className="text-fg-3"> / {formatInteger(item.total_runs)}</span>
        {item.failed_runs > 0 ? (
          <span className="text-neg"> · {formatInteger(item.failed_runs)} failed</span>
        ) : null}
      </span>
    ),
  },
  {
    id: "difference",
    header: "Mean paired difference",
    title:
      "First compared metric: mean of candidate minus baseline with its 95% Student t interval",
    align: "right",
    width: "minmax(350px, 1.8fr)",
    cell: (item) => <DifferenceCell headline={item.headline} />,
  },
  {
    id: "status",
    header: "Status",
    width: "164px",
    cell: (item) => <StatusIndicator spec={EXPERIMENT_STATUS[item.status]} />,
  },
  {
    id: "processing",
    header: "Processing",
    title: "Wall-clock time. Not simulation time.",
    align: "right",
    width: "96px",
    cell: (item) => <span className="num text-fg-3">{formatProcessing(item.processing_ms)}</span>,
  },
  {
    id: "created",
    header: "Created",
    unit: "UTC",
    align: "right",
    width: "168px",
    cell: (item) => <span className="num text-fg-3">{formatWallClock(item.created_at)}</span>,
  },
];

export function ExperimentsPage() {
  const navigate = useNavigate();
  const experiments = useQuery(experimentsQuery(500));
  const rows = useArrayRows(experiments.data?.experiments ?? []);
  return (
    <Page
      context={
        <ContextBar crumbs={[{ label: "Experiments" }, { label: "Stored experiments" }]}>
          <Button size="sm" variant="primary" onClick={() => void navigate("/experiments/new")}>
            New experiment
          </Button>
        </ContextBar>
      }
    >
      <InstrumentPanel
        index="01"
        title="Stored experiments"
        subtitle={
          experiments.data
            ? `${formatInteger(experiments.data.total)} in this workspace`
            : undefined
        }
        className="h-full"
      >
        {experiments.isPending ? (
          <LoadingState rows={10} label="Loading experiments" />
        ) : experiments.isError ? (
          <ErrorState error={experiments.error} onRetry={() => void experiments.refetch()} />
        ) : (
          <DataTable
            label="Stored experiments"
            columns={COLUMNS}
            {...rows}
            getRowId={(item) => item.id}
            onSelect={(item) => void navigate(`/experiments/${item.id}`)}
            onActivate={(item) => void navigate(`/experiments/${item.id}`)}
            empty={
              <EmptyState
                title="No experiments stored"
                actions={
                  <Button
                    size="sm"
                    variant="primary"
                    onClick={() => void navigate("/experiments/new")}
                  >
                    New experiment
                  </Button>
                }
              >
                An experiment runs two or more strategy variants over the same seeds and reports the
                paired difference of each metric with its confidence intervals. Start with the maker
                comparison on the baseline scenario.
              </EmptyState>
            }
          />
        )}
      </InstrumentPanel>
    </Page>
  );
}
