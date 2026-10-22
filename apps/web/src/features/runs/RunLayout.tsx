import { useQuery } from "@tanstack/react-query";
import { useState } from "react";
import { Outlet, useLocation, useNavigate, useOutletContext, useParams } from "react-router";

import { ContextBar, ViewTabs } from "@/components/shell/ContextBar";
import { Page } from "@/components/shell/TerminalShell";
import { Button } from "@/components/ui/Button";
import { InstrumentPanel } from "@/components/ui/InstrumentPanel";
import { KeyValue } from "@/components/ui/KeyValue";
import { EmptyState, ErrorState, LoadingState } from "@/components/ui/States";
import {
  REPLAY_STATUS,
  RUN_STATUS,
  StatusIndicator,
  type StatusSpec,
} from "@/components/ui/StatusIndicator";
import { runQuery } from "@/lib/api/queries";
import type { RunDetail } from "@/lib/api/types";
import { formatSpan, formatWallClock } from "@/lib/formatting";

import { ProvenanceDrawer } from "./ProvenanceDrawer";
import { scenarioLabel } from "./runColumns";

export interface RunContext {
  run: RunDetail;
}

export function useRun(): RunDetail {
  return useOutletContext<RunContext>().run;
}

const CELL_MATCH: StatusSpec = { label: "Cell digests match", tone: "positive", glyph: "dot" };
const SYNTHETIC: StatusSpec = {
  label: "Synthetic simulation",
  tone: "neutral",
  glyph: "square",
};
const CELL_DIFFER: StatusSpec = {
  label: "Cell digests differ",
  tone: "negative",
  glyph: "diamond",
};

const FORK_VERIFIED: StatusSpec = { label: "Fork prefix verified", tone: "positive", glyph: "dot" };
const FORK_FAILED: StatusSpec = { label: "Fork not verified", tone: "negative", glyph: "cross" };
const FORK_PENDING: StatusSpec = { label: "Fork", tone: "muted", glyph: "ring" };

/** The frame of one run: its identity and provenance stay in view across its screens. */
export function RunLayout() {
  const { runId = "" } = useParams();
  const location = useLocation();
  const navigate = useNavigate();
  const [provenance, setProvenance] = useState(false);
  const query = useQuery(runQuery(runId));
  const run = query.data;
  const base = `/runs/${runId}`;
  const view = location.pathname.startsWith(`${base}/agents`)
    ? "agents"
    : location.pathname.startsWith(`${base}/replay`)
      ? "replay"
      : location.pathname.startsWith(`${base}/timeline`)
        ? "timeline"
        : location.pathname.startsWith(`${base}/desk`)
          ? "desk"
          : "market";

  const context = (
    <ContextBar
      crumbs={[
        { label: "Simulation", to: "/simulations" },
        ...(run
          ? [
              { label: run.name, verbatim: true },
              { label: scenarioLabel(run) },
              { label: `Seed ${run.seed}` },
              { label: `Engine ${run.engine_version}` },
            ]
          : [{ label: runId, verbatim: true }]),
      ]}
      status={
        run ? (
          <>
            <StatusIndicator spec={SYNTHETIC} />
            <StatusIndicator spec={RUN_STATUS[run.status]} />
            {run.status === "completed" ? (
              <StatusIndicator spec={REPLAY_STATUS[run.replay.status]} />
            ) : null}
            {run.parent_run_id ? (
              <StatusIndicator
                spec={
                  run.fork_status === "verified"
                    ? FORK_VERIFIED
                    : run.fork_status === "failed"
                      ? FORK_FAILED
                      : FORK_PENDING
                }
              />
            ) : null}
            {run.cell && run.cell.digests_match !== null ? (
              <StatusIndicator spec={run.cell.digests_match ? CELL_MATCH : CELL_DIFFER} />
            ) : null}
          </>
        ) : undefined
      }
    >
      {run?.status === "completed" ? (
        <ViewTabs
          label="Run views"
          tabs={[
            { to: base, label: "Microscope", active: view === "market" },
            { to: `${base}/agents`, label: "Agents", active: view === "agents" },
            { to: `${base}/replay`, label: "Replay", active: view === "replay" },
            { to: `${base}/timeline`, label: "Timelines", active: view === "timeline" },
            ...(run.desk
              ? [{ to: `${base}/desk`, label: "AI Desk", active: view === "desk" }]
              : []),
          ]}
        />
      ) : null}
      {run ? (
        <Button size="sm" onClick={() => setProvenance(true)}>
          Provenance
        </Button>
      ) : null}
    </ContextBar>
  );

  let body;
  if (query.isPending) {
    body = <LoadingState label="Loading run" />;
  } else if (query.isError) {
    body = <ErrorState error={query.error} onRetry={() => void query.refetch()} />;
  } else if (run && (run.status === "pending" || run.status === "running")) {
    body = (
      <div className="hairline-grid grid grid-cols-1 lg:h-full lg:grid-cols-[minmax(0,1fr)_420px]">
        <InstrumentPanel index="01" title={run.status === "pending" ? "Queued" : "Running"}>
          <EmptyState
            title={
              run.status === "pending"
                ? "Waiting for a worker"
                : "The engine is running this simulation"
            }
          >
            The core runs a simulation as one native call and reports nothing until it returns, so
            no progress is shown: there is none to observe. This page updates when the run
            completes. Most one-hour scenarios finish in under a second.
          </EmptyState>
        </InstrumentPanel>
        <InstrumentPanel index="02" title="Submitted">
          <KeyValue
            rows={[
              { label: "Run", value: run.name },
              { label: "Scenario", value: scenarioLabel(run) },
              { label: "Seed", value: run.seed },
              { label: "Simulated horizon", value: formatSpan(run.horizon_ns) },
              { label: "Submitted", value: formatWallClock(run.created_at) },
              { label: "Status", value: <StatusIndicator spec={RUN_STATUS[run.status]} /> },
            ]}
          />
        </InstrumentPanel>
      </div>
    );
  } else if (run && run.status === "failed") {
    body = (
      <div className="hairline-grid grid grid-cols-1 lg:h-full lg:grid-cols-[minmax(0,1fr)_minmax(0,1fr)]">
        <InstrumentPanel index="01" title="Run failed" subtitle="no artifacts were published">
          <div role="alert" className="flex flex-col gap-3 p-4">
            <div>
              <p className="label text-fg-3">Engine error</p>
              <p className="num text-md font-medium text-neg">{run.error?.code ?? "RunFailed"}</p>
              <p className="mt-1 text-base text-fg">{run.error?.message}</p>
            </div>
            <KeyValue
              className="border-t border-line-subtle"
              rows={[
                {
                  label: "Trace sequence",
                  value: run.error?.sequence
                    ? run.error.sequence.toLocaleString("en-US")
                    : "not at a record",
                  title: "The record the engine was writing when it stopped",
                },
                { label: "Submitted", value: formatWallClock(run.created_at) },
                { label: "Failed", value: formatWallClock(run.finished_at) },
                { label: "Engine", value: run.engine_version },
              ]}
            />
            <p className="text-sm text-fg-3">
              A run that fails part-way publishes no manifest, so there is nothing to inspect or
              replay. The config that was submitted is on the right.
            </p>
            <div>
              <Button
                variant="primary"
                onClick={() =>
                  void navigate(
                    run.scenario_key
                      ? `/simulations/new?scenario=${run.scenario_key}`
                      : "/simulations/new",
                  )
                }
              >
                Configure a new run
              </Button>
            </div>
          </div>
        </InstrumentPanel>
        <InstrumentPanel index="02" title="Submitted config" subtitle="canonical form">
          <pre
            tabIndex={0}
            className="num h-full overflow-auto px-3 py-2 text-xs leading-relaxed whitespace-pre text-fg-2"
          >
            {JSON.stringify(run.config, null, 2)}
          </pre>
        </InstrumentPanel>
      </div>
    );
  } else if (run) {
    body = <Outlet context={{ run } satisfies RunContext} />;
  }

  return (
    <Page context={context}>
      {body}
      {run ? <ProvenanceDrawer run={run} open={provenance} onOpenChange={setProvenance} /> : null}
    </Page>
  );
}
