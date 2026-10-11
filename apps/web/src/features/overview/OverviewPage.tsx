import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { useNavigate } from "react-router";

import { ContextBar } from "@/components/shell/ContextBar";
import { Page } from "@/components/shell/TerminalShell";
import { DataTable, useArrayRows, type DataColumn } from "@/components/tables/DataTable";
import { Button } from "@/components/ui/Button";
import { InstrumentPanel } from "@/components/ui/InstrumentPanel";
import { MetricReadout } from "@/components/ui/MetricReadout";
import { EmptyState, ErrorState, LoadingState } from "@/components/ui/States";
import { EXPERIMENT_STATUS, StatusIndicator } from "@/components/ui/StatusIndicator";
import { DifferenceCell } from "@/features/experiments/DifferenceCell";
import { RUN_COLUMNS } from "@/features/runs/runColumns";
import { post } from "@/lib/api/client";
import { experimentsQuery, healthQuery, runsQuery, scenariosQuery } from "@/lib/api/queries";
import type { ExperimentSummary, ReplayView, RunSummary, ScenarioSummary } from "@/lib/api/types";
import { formatInteger, formatSpan } from "@/lib/formatting";

const RUN_TABLE: DataColumn<RunSummary>[] = [
  RUN_COLUMNS.name,
  RUN_COLUMNS.scenario,
  RUN_COLUMNS.seed,
  RUN_COLUMNS.duration,
  RUN_COLUMNS.trades,
  RUN_COLUMNS.status,
  RUN_COLUMNS.replay,
  RUN_COLUMNS.created,
];

const EXPERIMENT_TABLE: DataColumn<ExperimentSummary>[] = [
  {
    id: "experiment",
    header: "Experiment",
    width: "minmax(150px, 1.2fr)",
    cell: (item) => (
      <span className="flex min-w-0 items-baseline gap-2">
        <span className="truncate font-medium text-fg">{item.name}</span>
        <span className="num shrink-0 text-xs text-fg-3">{item.id}</span>
      </span>
    ),
  },
  {
    id: "comparison",
    header: "Comparison",
    title: "Candidate minus baseline, paired by seed",
    width: "minmax(170px, 1.3fr)",
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
    width: "104px",
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
    width: "minmax(330px, 1.8fr)",
    cell: (item) => <DifferenceCell headline={item.headline} />,
  },
  {
    id: "status",
    header: "Status",
    width: "132px",
    cell: (item) => <StatusIndicator spec={EXPERIMENT_STATUS[item.status]} />,
  },
];

function ScenarioList({ scenarios }: { scenarios: ScenarioSummary[] }) {
  const navigate = useNavigate();
  return (
    <ul className="h-full overflow-auto">
      {scenarios.map((scenario) => (
        <li key={scenario.key} className="border-b border-line-subtle">
          <button
            type="button"
            onClick={() => void navigate(`/simulations/new?scenario=${scenario.key}`)}
            className="transition-hover flex w-full flex-col gap-1 px-3 py-2 text-left hover:bg-surface-hover"
          >
            <span className="flex items-baseline gap-2">
              <span className="text-base font-medium text-fg">{scenario.title}</span>
              <span className="flex-1" />
              <span className="num text-xs text-fg-3">{formatSpan(scenario.horizon_ns)}</span>
            </span>
            <span className="text-xs leading-snug text-fg-3">{scenario.description}</span>
            <span className="label flex flex-wrap gap-x-3 gap-y-0.5 pt-0.5 text-fg-3">
              <span>
                <span className="num tracking-normal text-fg-2">{scenario.agent_count}</span> agents
              </span>
              <span>
                seed <span className="num tracking-normal text-fg-2">{scenario.seed}</span>
              </span>
              {scenario.shocks.length > 0 ? (
                <span className="text-warn">
                  {scenario.shocks.join(" · ").replace(/([a-z])([A-Z])/g, "$1 $2")}
                </span>
              ) : (
                <span>no shock</span>
              )}
              {scenario.has_accounting ? <span>fees</span> : null}
              {scenario.kind === "scripted" ? <span>scripted</span> : null}
            </span>
          </button>
        </li>
      ))}
    </ul>
  );
}

export function OverviewPage() {
  const navigate = useNavigate();
  const client = useQueryClient();
  const health = useQuery(healthQuery());
  const runs = useQuery(runsQuery(50, false));
  const experiments = useQuery(experimentsQuery(50));
  const scenarios = useQuery(scenariosQuery());

  const runRows = useArrayRows(runs.data?.runs ?? []);
  const experimentRows = useArrayRows(experiments.data?.experiments ?? []);
  const latest = runs.data?.runs.find((run) => run.status === "completed");

  const verify = useMutation({
    mutationFn: (runId: string) => post<ReplayView>(`/runs/${runId}/replay`),
    onSuccess: (_, runId) => {
      void client.invalidateQueries({ queryKey: ["runs"] });
      void navigate(`/runs/${runId}/replay`);
    },
  });

  const data = health.data;
  const empty = runs.isSuccess && runs.data.total === 0 && (experiments.data?.total ?? 0) === 0;

  return (
    <Page
      context={
        <ContextBar
          crumbs={[
            { label: "Overview" },
            { label: `Workspace ${data?.workspace ?? "—"}` },
            { label: `Engine ${data?.engine_version ?? "—"}` },
          ]}
        >
          <Button
            size="sm"
            disabled={!latest}
            onClick={() => latest && void navigate(`/runs/${latest.id}`)}
          >
            Open latest run
          </Button>
          <Button
            size="sm"
            disabled={!latest || verify.isPending}
            onClick={() => latest && verify.mutate(latest.id)}
            title="Regenerate the latest completed run and compare every record"
          >
            Verify replay
          </Button>
          <Button size="sm" onClick={() => void navigate("/experiments/new")}>
            New experiment
          </Button>
          <Button size="sm" variant="primary" onClick={() => void navigate("/simulations/new")}>
            New simulation
          </Button>
        </ContextBar>
      }
    >
      <div className="hairline-grid grid grid-cols-1 lg:h-full lg:grid-cols-[minmax(0,1fr)_340px] lg:grid-rows-[auto_minmax(0,1fr)]">
        <section
          aria-label="Research status"
          className="col-span-full flex flex-wrap items-stretch bg-surface-0"
        >
          {health.isError ? (
            <ErrorState error={health.error} compact onRetry={() => void health.refetch()} />
          ) : (
            <>
              <Stat
                label="Scenarios"
                value={formatInteger(data?.counts.scenarios)}
                note="shipped configs"
              />
              <Stat
                label="Stored runs"
                value={formatInteger(data?.counts.runs)}
                note="in this workspace"
              />
              <Stat
                label="Experiments"
                value={formatInteger(data?.counts.experiments)}
                note="paired designs"
              />
              <Stat
                label="Active jobs"
                value={formatInteger(data?.active_jobs)}
                note={data ? `of ${data.max_concurrent_jobs} concurrent` : undefined}
              />
              <Stat
                label="Engine"
                value={data?.engine_version ?? "—"}
                note={data ? `${data.build_type} · ${data.git_revision}` : undefined}
              />
              <Stat
                label="Longest horizon"
                value={data ? formatSpan(data.limits.max_horizon_ns) : "—"}
                note="per run, simulated"
              />
            </>
          )}
        </section>

        <div className="hairline-grid grid min-h-0 grid-rows-[420px_300px] lg:grid-rows-[minmax(0,3fr)_minmax(0,2fr)]">
          <InstrumentPanel
            index="01"
            title="Recent simulations"
            subtitle={runs.data ? `${formatInteger(runs.data.total)} stored` : undefined}
            actions={
              <Button size="sm" variant="ghost" onClick={() => void navigate("/simulations")}>
                All runs
              </Button>
            }
          >
            {runs.isPending ? (
              <LoadingState rows={8} label="Loading runs" />
            ) : runs.isError ? (
              <ErrorState error={runs.error} onRetry={() => void runs.refetch()} />
            ) : empty ? (
              <FirstRun />
            ) : (
              <DataTable
                label="Recent simulations"
                columns={RUN_TABLE}
                {...runRows}
                getRowId={(run) => run.id}
                onSelect={(run) => void navigate(`/runs/${run.id}`)}
                onActivate={(run) => void navigate(`/runs/${run.id}`)}
                empty={
                  <EmptyState
                    title="No simulations stored"
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
                    Runs you launch appear here with their seed, digest, and replay status.
                  </EmptyState>
                }
              />
            )}
          </InstrumentPanel>

          <InstrumentPanel
            index="02"
            title="Recent experiments"
            subtitle={
              experiments.data ? `${formatInteger(experiments.data.total)} stored` : undefined
            }
            actions={
              <Button size="sm" variant="ghost" onClick={() => void navigate("/experiments")}>
                All experiments
              </Button>
            }
          >
            {experiments.isPending ? (
              <LoadingState rows={4} label="Loading experiments" />
            ) : experiments.isError ? (
              <ErrorState error={experiments.error} onRetry={() => void experiments.refetch()} />
            ) : (
              <DataTable
                label="Recent experiments"
                columns={EXPERIMENT_TABLE}
                {...experimentRows}
                getRowId={(item) => item.id}
                onSelect={(item) => void navigate(`/experiments/${item.id}`)}
                onActivate={(item) => void navigate(`/experiments/${item.id}`)}
                empty={
                  <EmptyState
                    title="No experiments stored"
                    actions={
                      <Button size="sm" onClick={() => void navigate("/experiments/new")}>
                        New experiment
                      </Button>
                    }
                  >
                    An experiment runs strategy variants over matched seeds and reports paired
                    differences with their confidence intervals.
                  </EmptyState>
                }
              />
            )}
          </InstrumentPanel>
        </div>

        <InstrumentPanel
          index="03"
          title="Scenarios"
          subtitle="select to configure a run"
          className="hidden lg:flex"
        >
          {scenarios.isPending ? (
            <LoadingState rows={6} label="Loading scenarios" />
          ) : scenarios.isError ? (
            <ErrorState error={scenarios.error} compact onRetry={() => void scenarios.refetch()} />
          ) : (
            <ScenarioList scenarios={scenarios.data} />
          )}
        </InstrumentPanel>
      </div>
    </Page>
  );
}

function Stat({ label, value, note }: { label: string; value: string; note?: string }) {
  return (
    <div className="min-w-[132px] flex-1 border-r border-line-subtle px-4 py-2.5 last:border-r-0">
      <MetricReadout label={label} value={value} note={note} size="lg" />
    </div>
  );
}

function FirstRun() {
  const navigate = useNavigate();
  return (
    <div className="flex h-full flex-col justify-center gap-4 px-8 py-6">
      <div>
        <p className="label text-fg-2">This workspace has no runs yet</p>
        <p className="mt-1 max-w-[70ch] text-sm text-fg-3">
          Nothing is shown until the engine has produced it. Every chart, table, and statistic in
          this terminal is read from the artifacts of a run in this workspace.
        </p>
      </div>
      <ol className="grid max-w-[880px] grid-cols-1 gap-px bg-line-subtle md:grid-cols-4">
        {[
          [
            "Choose a scenario",
            "Six shipped configs: a baseline hour, three shocks, a fee schedule, and a scripted demo.",
          ],
          [
            "Run it",
            "The C++ core simulates one hour of market in under a second and writes six artifacts.",
          ],
          [
            "Inspect it",
            "Scrub the timeline: price, depth, agents, and events move together, as recorded.",
          ],
          [
            "Verify it",
            "Replay regenerates the run and compares every command, output, and ledger entry.",
          ],
        ].map(([title, text], index) => (
          <li key={title} className="flex flex-col gap-1 bg-surface-0 p-3">
            <span className="num text-xs text-accent-label">0{index + 1}</span>
            <span className="text-base font-medium text-fg">{title}</span>
            <span className="text-xs leading-snug text-fg-3">{text}</span>
          </li>
        ))}
      </ol>
      <div className="flex gap-2">
        <Button variant="solid" onClick={() => void navigate("/simulations/new?scenario=baseline")}>
          Configure the baseline run
        </Button>
        <Button onClick={() => void navigate("/experiments/new")}>Set up an experiment</Button>
      </div>
    </div>
  );
}
