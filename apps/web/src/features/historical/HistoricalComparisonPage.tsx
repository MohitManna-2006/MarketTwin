import { useQuery } from "@tanstack/react-query";
import { useState } from "react";

import { ContextBar } from "@/components/shell/ContextBar";
import { Page } from "@/components/shell/TerminalShell";
import { SelectField } from "@/components/ui/Field";
import { InstrumentPanel } from "@/components/ui/InstrumentPanel";
import { ErrorState, LoadingState } from "@/components/ui/States";
import { historicalComparisonQuery, historicalImportsQuery, runsQuery } from "@/lib/api/queries";

export function HistoricalComparisonPage() {
  const runs = useQuery(runsQuery(100, false));
  const sessions = useQuery(historicalImportsQuery());
  const completedRuns = runs.data?.runs.filter((run) => run.status === "completed") ?? [];
  const completedSessions =
    sessions.data?.imports.filter((session) => session.status === "completed") ?? [];
  const [runId, setRunId] = useState("");
  const [sessionId, setSessionId] = useState("");
  const selectedRun = runId || completedRuns[0]?.id || "";
  const selectedSession = sessionId || completedSessions[0]?.id || "";
  const canCompare = Boolean(selectedRun && selectedSession);
  const comparison = useQuery(historicalComparisonQuery(selectedRun, selectedSession, canCompare));

  return (
    <Page
      scroll
      context={
        <ContextBar
          crumbs={[{ label: "Historical", to: "/historical" }, { label: "Synthetic comparison" }]}
        />
      }
    >
      <div className="grid min-h-full grid-cols-1 gap-px bg-line-subtle lg:grid-cols-[300px_1fr]">
        <InstrumentPanel index="01" title="Comparison inputs" className="min-h-[320px]">
          <div className="flex flex-col gap-4 p-4">
            <SelectField
              label="Synthetic MarketTwin run"
              value={selectedRun}
              onChange={setRunId}
              options={completedRuns.map((run) => ({
                value: run.id,
                label: `${run.name} · seed ${run.seed}`,
              }))}
            />
            <SelectField
              label="Observed historical session"
              value={selectedSession}
              onChange={setSessionId}
              options={completedSessions.map((session) => ({
                value: session.id,
                label: `${session.symbol} · ${session.provider}/${session.feed}`,
              }))}
            />
            <p className="border-l-2 border-warn bg-surface-1 p-3 text-sm text-fg-2">
              This is a diagnostic comparison. It is not calibration, backtesting, paper execution,
              or evidence that synthetic agents represent actual market participants.
            </p>
          </div>
        </InstrumentPanel>
        <InstrumentPanel
          index="02"
          title="Definition-aware statistics"
          subtitle="incompatible values are explicitly marked"
          className="min-h-[320px]"
          bodyClassName="overflow-auto"
        >
          {runs.isPending || sessions.isPending || (canCompare && comparison.isPending) ? (
            <LoadingState rows={6} label="Loading comparison" />
          ) : runs.isError || sessions.isError || comparison.isError ? (
            <ErrorState error={runs.error ?? sessions.error ?? comparison.error} />
          ) : comparison.data ? (
            <>
              <table className="w-full text-left text-sm">
                <thead className="label bg-surface-1 text-fg-3">
                  <tr>
                    <th className="p-3">Metric</th>
                    <th className="p-3">Synthetic</th>
                    <th className="p-3">Historical</th>
                    <th className="p-3">Comparability</th>
                  </tr>
                </thead>
                <tbody>
                  {comparison.data.metrics.map((metric) => (
                    <tr key={metric.name} className="border-t border-line-subtle">
                      <td className="p-3">
                        <div className="label text-fg">{metric.name.replaceAll("_", " ")}</div>
                        <div className="mt-1 text-xs text-fg-3">{metric.unit}</div>
                      </td>
                      <td className="num p-3 text-fg-2">{metric.synthetic ?? "—"}</td>
                      <td className="num p-3 text-fg-2">{metric.historical ?? "—"}</td>
                      <td className="p-3">
                        <span className={metric.compatible ? "text-pos" : "text-warn"}>
                          {metric.compatible
                            ? "Conditionally compatible"
                            : "Not directly comparable"}
                        </span>
                        <p className="mt-1 max-w-[58ch] text-xs text-fg-3">{metric.note}</p>
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
              <div className="border-t border-line p-4">
                <p className="label text-fg-3">Methodology limits</p>
                <ul className="mt-2 list-disc space-y-1 pl-5 text-sm text-fg-2">
                  {comparison.data.limitations.map((limit) => (
                    <li key={limit}>{limit}</li>
                  ))}
                </ul>
              </div>
            </>
          ) : (
            <p className="p-4 text-sm text-fg-3">
              Complete one synthetic run and one historical import to compare them.
            </p>
          )}
        </InstrumentPanel>
      </div>
    </Page>
  );
}
