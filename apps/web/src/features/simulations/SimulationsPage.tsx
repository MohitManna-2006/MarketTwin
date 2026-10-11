import { useQuery } from "@tanstack/react-query";
import { useMemo, useState } from "react";
import { useNavigate } from "react-router";

import { ContextBar } from "@/components/shell/ContextBar";
import { Page } from "@/components/shell/TerminalShell";
import { DataTable, useArrayRows, type DataColumn } from "@/components/tables/DataTable";
import { Button } from "@/components/ui/Button";
import { Checkbox } from "@/components/ui/Field";
import { InstrumentPanel } from "@/components/ui/InstrumentPanel";
import { EmptyState, ErrorState, LoadingState } from "@/components/ui/States";
import { TableFilter } from "@/components/ui/TableFilter";
import { RUN_COLUMNS, scenarioLabel } from "@/features/runs/runColumns";
import { runsQuery } from "@/lib/api/queries";
import type { RunSummary } from "@/lib/api/types";
import { formatInteger } from "@/lib/formatting";

const COLUMNS: DataColumn<RunSummary>[] = [
  RUN_COLUMNS.name,
  RUN_COLUMNS.scenario,
  RUN_COLUMNS.seed,
  RUN_COLUMNS.duration,
  RUN_COLUMNS.trades,
  RUN_COLUMNS.status,
  RUN_COLUMNS.replay,
  RUN_COLUMNS.processing,
  RUN_COLUMNS.created,
];

export function SimulationsPage() {
  const navigate = useNavigate();
  const [cells, setCells] = useState(false);
  const [filter, setFilter] = useState("");
  const runs = useQuery(runsQuery(1000, cells));

  const filtered = useMemo(() => {
    const needle = filter.trim().toLowerCase();
    const all = runs.data?.runs ?? [];
    if (!needle) return all;
    return all.filter((run) =>
      [run.name, run.id, scenarioLabel(run), String(run.seed), run.status, run.config_digest]
        .join(" ")
        .toLowerCase()
        .includes(needle),
    );
  }, [runs.data, filter]);
  const rows = useArrayRows(filtered);

  return (
    <Page
      context={
        <ContextBar crumbs={[{ label: "Simulations" }, { label: "Stored runs" }]}>
          <Button size="sm" variant="primary" onClick={() => void navigate("/simulations/new")}>
            New simulation
          </Button>
        </ContextBar>
      }
    >
      <InstrumentPanel
        index="01"
        title="Stored runs"
        subtitle={
          runs.data
            ? `${formatInteger(filtered.length)} shown of ${formatInteger(runs.data.total)}`
            : undefined
        }
        actions={
          <>
            <Checkbox label="Include experiment cells" checked={cells} onChange={setCells} />
            <TableFilter
              value={filter}
              onChange={setFilter}
              placeholder="Name, ID, scenario, seed, digest"
            />
          </>
        }
        className="h-full"
      >
        {runs.isPending ? (
          <LoadingState rows={12} label="Loading runs" />
        ) : runs.isError ? (
          <ErrorState error={runs.error} onRetry={() => void runs.refetch()} />
        ) : (
          <DataTable
            label="Stored runs"
            columns={COLUMNS}
            {...rows}
            sortable
            getRowId={(run) => run.id}
            onSelect={(run) => void navigate(`/runs/${run.id}`)}
            onActivate={(run) => void navigate(`/runs/${run.id}`)}
            empty={
              runs.data.total === 0 ? (
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
                  Configure a scenario and run it. The run appears here with its seed, config
                  digest, and replay status, and can be reopened at any time.
                </EmptyState>
              ) : (
                <EmptyState title="No run matches the filter">
                  Clear the filter to see all {formatInteger(runs.data.total)} stored runs.
                </EmptyState>
              )
            }
          />
        )}
      </InstrumentPanel>
    </Page>
  );
}
