import type { DataColumn } from "@/components/tables/DataTable";
import {
  REPLAY_STATUS,
  REPLAY_STATUS_SHORT,
  RUN_STATUS,
  StatusIndicator,
} from "@/components/ui/StatusIndicator";
import type { RunSummary } from "@/lib/api/types";
import { formatInteger, formatProcessing, formatSpan, formatWallClock } from "@/lib/formatting";

export function scenarioLabel(run: Pick<RunSummary, "scenario_key" | "source_kind">): string {
  if (run.scenario_key) return run.scenario_key.replaceAll("_", " ");
  return "custom";
}

const name: DataColumn<RunSummary> = {
  id: "run",
  header: "Run",
  width: "minmax(190px, 1.6fr)",
  sortValue: (run) => run.name,
  cell: (run) => (
    <span className="flex min-w-0 items-baseline gap-2">
      <span className="truncate font-medium text-fg">{run.name}</span>
      <span className="num shrink-0 text-xs text-fg-3">{run.id}</span>
    </span>
  ),
};

const scenario: DataColumn<RunSummary> = {
  id: "scenario",
  header: "Scenario",
  width: "minmax(120px, 1fr)",
  sortValue: (run) => run.scenario_key ?? "",
  cell: (run) => (
    <span className="text-fg-2">
      {scenarioLabel(run)}
      {run.source_kind === "experiment_cell" ? (
        <span className="text-fg-3"> · cell of {run.experiment_id}</span>
      ) : null}
    </span>
  ),
};

const seed: DataColumn<RunSummary> = {
  id: "seed",
  header: "Seed",
  align: "right",
  width: "72px",
  sortValue: (run) => run.seed,
  cell: (run) => <span className="num text-fg-2">{run.seed}</span>,
};

const duration: DataColumn<RunSummary> = {
  id: "duration",
  header: "Simulated",
  title: "Elapsed simulation time the run covered. Not wall-clock time.",
  align: "right",
  width: "96px",
  sortValue: (run) => run.final_time_ns ?? run.horizon_ns,
  cell: (run) => (
    <span className="num text-fg-2">
      {run.final_time_ns !== null ? formatSpan(run.final_time_ns) : formatSpan(run.horizon_ns)}
    </span>
  ),
};

const trades: DataColumn<RunSummary> = {
  id: "trades",
  header: "Trades",
  align: "right",
  width: "84px",
  sortValue: (run) => run.trade_count,
  cell: (run) => <span className="num text-fg">{formatInteger(run.trade_count)}</span>,
};

const status: DataColumn<RunSummary> = {
  id: "status",
  header: "Status",
  width: "118px",
  sortValue: (run) => run.status,
  cell: (run) => (
    <span title={run.error ? `${run.error.code}: ${run.error.message}` : undefined}>
      <StatusIndicator spec={RUN_STATUS[run.status]} />
    </span>
  ),
};

const replay: DataColumn<RunSummary> = {
  id: "replay",
  header: "Replay",
  width: "118px",
  sortValue: (run) => run.replay_status,
  cell: (run) =>
    run.status === "completed" ? (
      <StatusIndicator
        spec={REPLAY_STATUS[run.replay_status]}
        label={REPLAY_STATUS_SHORT[run.replay_status]}
      />
    ) : (
      <span className="num text-fg-3">—</span>
    ),
};

const processing: DataColumn<RunSummary> = {
  id: "processing",
  header: "Processing",
  title: "Wall-clock time the engine took. Not simulation time.",
  align: "right",
  width: "92px",
  sortValue: (run) => run.processing_ms,
  cell: (run) => <span className="num text-fg-3">{formatProcessing(run.processing_ms)}</span>,
};

const created: DataColumn<RunSummary> = {
  id: "created",
  header: "Created",
  unit: "UTC",
  title: "Wall-clock time the run was submitted.",
  align: "right",
  width: "168px",
  sortValue: (run) => run.created_at,
  cell: (run) => <span className="num text-fg-3">{formatWallClock(run.created_at)}</span>,
};

export const RUN_COLUMNS = {
  name,
  scenario,
  seed,
  duration,
  trades,
  status,
  replay,
  processing,
  created,
};
