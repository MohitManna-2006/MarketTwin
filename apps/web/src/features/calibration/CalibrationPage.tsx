import { useMutation, useQuery } from "@tanstack/react-query";
import { useParams } from "react-router";

import { ContextBar } from "@/components/shell/ContextBar";
import { Page } from "@/components/shell/TerminalShell";
import { Button } from "@/components/ui/Button";
import { InstrumentPanel } from "@/components/ui/InstrumentPanel";
import { KeyValue } from "@/components/ui/KeyValue";
import { EmptyState, ErrorState, LoadingState } from "@/components/ui/States";
import { RUN_STATUS, StatusIndicator } from "@/components/ui/StatusIndicator";
import { post } from "@/lib/api/client";
import { calibrationQuery, calibrationReportQuery } from "@/lib/api/queries";
import type { CalibrationReport, CalibrationReportView, CalibrationScore } from "@/lib/api/types";
import { cx } from "@/lib/cx";

import {
  FACT_LABELS,
  FACT_ORDER,
  PARAMETER_LABELS,
  formatFact,
  formatParameter,
  formatScales,
} from "./format";

const ROLE_ORDER = ["train", "validation", "test"];

function scoreOf(score: CalibrationScore | null | undefined): string {
  if (!score) return "—";
  if (!score.valid) return `invalid: ${score.invalid_reason ?? "unknown"}`;
  return formatScales(score.score_ppm);
}

function Outcome({ report }: { report: CalibrationReport }) {
  const holds = report.outcome === "fit_holds";
  return (
    <section
      aria-label="Outcome"
      className={cx(
        "flex flex-col gap-2 border-l-4 bg-surface-1 p-4",
        holds ? "border-pos" : "border-warn",
      )}
    >
      <div className="label text-base text-fg">{report.label}</div>
      <div className="label text-xs text-fg-3">{report.observation_kind}</div>
      <p className="text-sm text-fg-2">{report.claim}</p>
      {report.null_reasons.length > 0 ? (
        <ul className="list-disc pl-5 text-sm text-fg-2">
          {report.null_reasons.map((reason) => (
            <li key={reason}>{reason}</li>
          ))}
        </ul>
      ) : null}
      <p className="text-xs text-fg-3">Not claimed: {report.not_claimed.join("; ")}.</p>
      {report.deviations_from_adr.length > 0 ? (
        <p className="text-xs text-fg-3">
          Departs from ADR 0011: {report.deviations_from_adr.join("; ")}.
        </p>
      ) : null}
    </section>
  );
}

function FactsTable({ view }: { view: CalibrationReportView }) {
  const { report } = view;
  const selected = report.test.selected?.facts ?? null;
  const baseline = report.test.baseline?.facts ?? null;
  const limit = report.acceptance.distance_ppm_limit;
  return (
    <table className="w-full border-collapse text-left text-sm">
      <thead className="sticky top-0 bg-surface-1">
        <tr className="label text-fg-3">
          {[
            "Fact",
            "Train target ± dev",
            "Scale",
            "Test target",
            "Selected ± seed dev",
            "d selected",
            "d baseline",
          ].map((label) => (
            <th key={label} className="border-b border-line px-3 py-2 font-medium">
              {label}
            </th>
          ))}
        </tr>
      </thead>
      <tbody>
        {FACT_ORDER.map((name) => {
          const chosen = selected?.[name];
          const base = baseline?.[name];
          const within =
            chosen?.distance_ppm !== null &&
            chosen?.distance_ppm !== undefined &&
            Math.abs(chosen.distance_ppm) <= limit;
          return (
            <tr key={name} className="border-b border-line-subtle">
              <td className="px-3 py-1.5 text-fg">
                <div>{FACT_LABELS[name] ?? name}</div>
                <div className="num text-xs text-fg-3">{view.groups[name]}</div>
              </td>
              <td className="num px-3 py-1.5 text-fg-2">
                {formatFact(name, report.targets.train.mean[name])} ±{" "}
                {formatFact(name, report.targets.train.deviation[name])}
              </td>
              <td className="num px-3 py-1.5 text-fg-3">
                {formatFact(name, report.targets.train.scale[name])}
              </td>
              <td className="num px-3 py-1.5 text-fg-2">
                {formatFact(name, report.targets.test.mean[name])}
              </td>
              <td className="num px-3 py-1.5 text-fg-2">
                {chosen
                  ? `${formatFact(name, chosen.mean)} ± ${formatFact(name, chosen.deviation)}`
                  : "—"}
              </td>
              <td className={cx("num px-3 py-1.5", within ? "text-pos" : "text-neg")}>
                {formatScales(chosen?.distance_ppm)}
                <span className="sr-only">
                  {within ? " within two scales" : " outside two scales"}
                </span>
              </td>
              <td className="num px-3 py-1.5 text-fg-3">{formatScales(base?.distance_ppm)}</td>
            </tr>
          );
        })}
      </tbody>
    </table>
  );
}

function Report({ view }: { view: CalibrationReportView }) {
  const { report } = view;
  const selected = report.selected;
  const finalist = report.stage2.find((entry) => entry.candidate === selected?.candidate);
  const quantiles = report.stage1.fit_ppm_quantiles;
  return (
    <div className="grid min-h-full grid-cols-1 gap-px bg-line-subtle xl:grid-cols-[minmax(0,1fr)_minmax(0,24rem)]">
      <div className="flex min-w-0 flex-col gap-px">
        <Outcome report={report} />
        <InstrumentPanel
          index="01"
          title="Stylized facts on the test sessions"
          subtitle="d = (selected − test target) / train scale"
          bodyClassName="overflow-auto"
        >
          <FactsTable view={view} />
        </InstrumentPanel>
        <InstrumentPanel index="03" title="Sessions" subtitle="split oldest to newest">
          <table className="w-full border-collapse text-left text-sm">
            <thead className="bg-surface-1">
              <tr className="label text-fg-3">
                {["Day", "Role", "Gate", "Session"].map((label) => (
                  <th key={label} className="border-b border-line px-3 py-2 font-medium">
                    {label}
                  </th>
                ))}
              </tr>
            </thead>
            <tbody>
              {[...report.sessions]
                .sort(
                  (left, right) =>
                    ROLE_ORDER.indexOf(left.role) - ROLE_ORDER.indexOf(right.role) ||
                    left.day.localeCompare(right.day),
                )
                .map((session) => (
                  <tr key={session.session_id} className="border-b border-line-subtle">
                    <td className="num px-3 py-1.5 text-fg">{session.day}</td>
                    <td className="label px-3 py-1.5 text-xs text-fg-2">{session.role}</td>
                    <td
                      className={cx(
                        "px-3 py-1.5 text-xs",
                        session.admitted ? "text-fg-2" : "text-neg",
                      )}
                    >
                      {session.admitted ? "admitted" : session.rejection_reasons.join("; ")}
                    </td>
                    <td className="num px-3 py-1.5 text-xs text-fg-3">{session.session_id}</td>
                  </tr>
                ))}
            </tbody>
          </table>
        </InstrumentPanel>
      </div>
      <div className="flex min-w-0 flex-col gap-px">
        <InstrumentPanel index="02" title="Acceptance" subtitle="fixed before any data">
          <KeyValue
            rows={[
              {
                label: "Facts within 2",
                value: `${report.acceptance.facts_within} of 12 (needs ${report.acceptance.facts_required})`,
              },
              { label: "Beats baseline", value: report.acceptance.beats_baseline ? "yes" : "no" },
              { label: "Selected, test", value: scoreOf(report.test.selected) },
              { label: "Baseline, test", value: scoreOf(report.test.baseline) },
              { label: "Selected, train", value: scoreOf(finalist?.fit) },
              { label: "Selected, validation", value: scoreOf(finalist?.validation) },
              { label: "Baseline, train", value: scoreOf(report.stage1.baseline_fit) },
            ]}
          />
        </InstrumentPanel>
        <InstrumentPanel
          index="04"
          title="Selected parameters"
          subtitle={selected ? `candidate ${selected.candidate}` : "none"}
        >
          {selected ? (
            <KeyValue
              rows={Object.keys(PARAMETER_LABELS).map((key) => ({
                label: PARAMETER_LABELS[key] ?? key,
                value: formatParameter(key, selected.parameters?.[key]),
              }))}
            />
          ) : (
            <p className="p-3 text-sm text-fg-3">No candidate was valid in stage 2.</p>
          )}
        </InstrumentPanel>
        <InstrumentPanel index="05" title="Search" subtitle="every candidate kept">
          <KeyValue
            rows={[
              {
                label: "Stage 1",
                value: `${report.stage1.valid} valid, ${report.stage1.invalid} invalid`,
              },
              { label: "Fit p10", value: formatScales(quantiles.p10) },
              { label: "Fit median", value: formatScales(quantiles.p50) },
              { label: "Fit p90", value: formatScales(quantiles.p90) },
              { label: "Finalists", value: report.stage1.finalists.join(", ") || "none" },
              {
                label: "Reference",
                value: `${(report.reference_price_cents / 100).toFixed(2)} USD`,
              },
              {
                label: "Seeds",
                value: `${report.seeds.stage1?.length ?? 0} / ${report.seeds.stage2?.length ?? 0} / ${report.seeds.test?.length ?? 0}`,
              },
            ]}
          />
        </InstrumentPanel>
      </div>
    </div>
  );
}

export function CalibrationPage() {
  const { calibrationId = "" } = useParams();
  const calibration = useQuery(calibrationQuery(calibrationId));
  const completed = calibration.data?.status === "completed";
  const report = useQuery(calibrationReportQuery(calibrationId, completed));
  const verify = useMutation({
    mutationFn: () =>
      post<{ verified: boolean; checks: { check: string; ok: boolean }[] }>(
        `/calibrations/${calibrationId}/verify`,
      ),
  });

  const row = calibration.data;
  return (
    <Page
      scroll
      context={
        <ContextBar
          crumbs={[{ label: "Calibration", to: "/calibration" }, { label: calibrationId }]}
        >
          {completed ? (
            <Button size="sm" disabled={verify.isPending} onClick={() => verify.mutate()}>
              {verify.isPending
                ? "Verifying…"
                : verify.data
                  ? verify.data.verified
                    ? "Verified"
                    : "Verification failed"
                  : "Verify artifact"}
            </Button>
          ) : null}
        </ContextBar>
      }
    >
      {calibration.isPending ? (
        <LoadingState rows={8} label="Loading calibration" />
      ) : calibration.isError ? (
        <ErrorState error={calibration.error} onRetry={() => void calibration.refetch()} />
      ) : row && row.status !== "completed" ? (
        <div className="h-full bg-surface-0">
          <EmptyState
            title={row.status === "failed" ? "Calibration failed" : "Calibration running"}
          >
            <span className="flex flex-col items-center gap-2">
              <StatusIndicator spec={RUN_STATUS[row.status]} />
              {row.progress ? (
                <span className="num text-xs">
                  {row.progress.phase} {row.progress.completed}/{row.progress.total}
                </span>
              ) : null}
              {row.error_code ? (
                <span className="num text-xs text-neg">
                  {row.error_code}: {row.error_message}
                </span>
              ) : null}
            </span>
          </EmptyState>
        </div>
      ) : report.isPending ? (
        <LoadingState rows={8} label="Loading report" />
      ) : report.isError ? (
        <ErrorState error={report.error} onRetry={() => void report.refetch()} />
      ) : (
        <Report view={report.data} />
      )}
      {verify.isError ? <ErrorState error={verify.error} compact /> : null}
    </Page>
  );
}
