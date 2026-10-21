import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { useMemo, useState } from "react";
import { useNavigate } from "react-router";

import { ContextBar } from "@/components/shell/ContextBar";
import { Page } from "@/components/shell/TerminalShell";
import { Button } from "@/components/ui/Button";
import { Checkbox, SelectField } from "@/components/ui/Field";
import { InstrumentPanel } from "@/components/ui/InstrumentPanel";
import { ErrorState, LoadingState } from "@/components/ui/States";
import { RUN_STATUS, StatusIndicator } from "@/components/ui/StatusIndicator";
import { post } from "@/lib/api/client";
import {
  calibrationSpecsQuery,
  calibrationsQuery,
  historicalImportsQuery,
} from "@/lib/api/queries";
import type { Calibration, CalibrationSpecSummary, HistoricalImport } from "@/lib/api/types";

import { formatScales } from "./format";

const PROVIDER_FOR_FEED: Record<string, string> = { iex: "alpaca", fabricated_l1: "local_file" };

/** Sessions a spec can use: its provider, feed, and symbol, and a window of its length. */
export function eligibleSessions(
  spec: CalibrationSpecSummary | undefined,
  imports: HistoricalImport[],
): HistoricalImport[] {
  if (!spec) return [];
  const [startH, startM] = spec.window.start_local.split(":").map(Number);
  const [endH, endM] = spec.window.end_local.split(":").map(Number);
  const length = ((endH ?? 0) * 60 + (endM ?? 0) - (startH ?? 0) * 60 - (startM ?? 0)) * 60_000;
  return imports
    .filter(
      (item) =>
        item.status === "completed" &&
        item.provider === PROVIDER_FOR_FEED[spec.feed] &&
        item.feed === spec.feed &&
        item.symbol === spec.symbol &&
        item.start_time !== null &&
        item.end_time !== null &&
        Date.parse(item.end_time) - Date.parse(item.start_time) === length,
    )
    .sort((left, right) => Date.parse(left.start_time ?? "") - Date.parse(right.start_time ?? ""));
}

function outcomeText(item: Calibration): string {
  if (item.status !== "completed" || !item.headline) return "";
  return item.headline.label;
}

export function CalibrationsPage() {
  const navigate = useNavigate();
  const cache = useQueryClient();
  const specs = useQuery(calibrationSpecsQuery());
  const calibrations = useQuery(calibrationsQuery());
  const imports = useQuery(historicalImportsQuery());
  const [specKey, setSpecKey] = useState("aapl_iex");
  const [chosen, setChosen] = useState<string[]>([]);

  const spec = specs.data?.specs.find((item) => item.key === specKey);
  const eligible = useMemo(
    () => eligibleSessions(spec, imports.data?.imports ?? []),
    [spec, imports.data],
  );
  const required = spec?.sessions_required ?? 0;

  const fixtures = useMutation({
    mutationFn: () => post<{ session_ids: string[] }>("/calibrations/fixture-sessions"),
    onSuccess: async (result) => {
      await cache.invalidateQueries({ queryKey: ["historical", "imports"] });
      setSpecKey("fixture_smoke");
      setChosen(result.session_ids);
    },
  });
  const submit = useMutation({
    mutationFn: () => post<Calibration>("/calibrations", { spec: specKey, session_ids: chosen }),
    onSuccess: async (row) => {
      await cache.invalidateQueries({ queryKey: ["calibrations", "list"] });
      void navigate(`/calibration/${row.id}`);
    },
  });

  function toggle(id: string, checked: boolean) {
    setChosen((current) =>
      checked
        ? [...current.filter((item) => item !== id), id]
        : current.filter((item) => item !== id),
    );
  }

  return (
    <Page scroll context={<ContextBar crumbs={[{ label: "Calibration" }, { label: "Fits" }]} />}>
      <div className="grid min-h-full grid-cols-1 gap-px bg-line-subtle lg:grid-cols-[minmax(0,26rem)_minmax(0,1fr)]">
        <InstrumentPanel
          index="01"
          title="New calibration"
          subtitle="ADR 0011, pre-registered"
          className="min-h-[420px]"
          bodyClassName="overflow-auto"
        >
          <form
            className="flex min-w-0 flex-col gap-4 p-4"
            onSubmit={(event) => {
              event.preventDefault();
              submit.mutate();
            }}
          >
            <p className="text-sm text-fg-2">
              Fits noise-trader, maker, and fundamental parameters so that twelve stylized facts of
              synthetic runs approach the same facts measured on observed sessions, then tests the
              fit once on sessions it never saw. Every fact and score is computed by the C++ core.
            </p>
            {specs.isError ? <ErrorState error={specs.error} compact /> : null}
            <SelectField
              label="Spec"
              value={specKey}
              onChange={(value) => {
                setSpecKey(value);
                setChosen([]);
              }}
              options={(specs.data?.specs ?? []).map((item) => ({
                value: item.key,
                label: item.title,
              }))}
            />
            {spec ? (
              <div className="flex flex-col gap-1 text-xs text-fg-3">
                <span className="num whitespace-normal">
                  {spec.symbol} · {spec.feed.toUpperCase()} · {spec.window.start_local}–
                  {spec.window.end_local} {spec.window.timezone} · {spec.split.train}/
                  {spec.split.validation}/{spec.split.test} sessions
                </span>
                {spec.deviations_from_adr.length > 0 ? (
                  <span className="border-l-2 border-warn pl-2 text-fg-2">
                    Smaller than ADR 0011: {spec.deviations_from_adr.join("; ")}. Fabricated data
                    only.
                  </span>
                ) : (
                  <span>Exactly the ADR 0011 design.</span>
                )}
                {spec.feed === "iex" ? (
                  <span className="border-l-2 border-warn pl-2 text-fg-2">
                    Import each day&apos;s 10:30–11:30 ET window from Alpaca IEX on the Historical
                    page first, or run services/api/scripts/calibrate_aapl.py.
                  </span>
                ) : null}
              </div>
            ) : null}
            <div className="flex flex-wrap gap-2">
              <Button
                size="sm"
                type="button"
                disabled={eligible.length < required}
                onClick={() => setChosen(eligible.slice(-required).map((item) => item.id))}
              >
                Select the {required} most recent
              </Button>
              <Button
                size="sm"
                type="button"
                disabled={fixtures.isPending}
                onClick={() => fixtures.mutate()}
              >
                {fixtures.isPending ? "Importing…" : "Import fabricated sessions"}
              </Button>
            </div>
            {fixtures.isError ? <ErrorState error={fixtures.error} compact /> : null}
            <div className="max-h-72 overflow-auto border border-line-subtle">
              {eligible.length === 0 ? (
                <p className="p-3 text-xs text-fg-3">
                  No completed {spec?.symbol ?? ""} {spec?.feed ?? ""} sessions with this window.
                </p>
              ) : (
                eligible.map((item) => (
                  <div key={item.id} className="border-b border-line-subtle px-2 py-1">
                    <Checkbox
                      label={`${item.start_time ?? ""} · ${item.id}`}
                      checked={chosen.includes(item.id)}
                      onChange={(checked) => toggle(item.id, checked)}
                    />
                  </div>
                ))
              )}
            </div>
            <span className="num text-xs whitespace-normal text-fg-3">
              {chosen.length} of {required} sessions selected; split oldest to newest into train,
              validation, and test.
            </span>
            <Button
              variant="solid"
              type="submit"
              disabled={
                submit.isPending || !spec || chosen.length !== required || spec.problems.length > 0
              }
              className="self-start"
            >
              {submit.isPending ? "Submitting…" : "Run calibration"}
            </Button>
            {submit.isError ? <ErrorState error={submit.error} compact /> : null}
          </form>
        </InstrumentPanel>

        <InstrumentPanel
          index="02"
          title="Calibrations"
          subtitle="pass and null results shown alike"
          className="min-h-[420px]"
          bodyClassName="overflow-auto"
        >
          {calibrations.isPending ? (
            <LoadingState rows={8} label="Loading calibrations" />
          ) : calibrations.isError ? (
            <ErrorState error={calibrations.error} onRetry={() => void calibrations.refetch()} />
          ) : (
            <table className="w-full border-collapse text-left text-sm">
              <thead className="sticky top-0 bg-surface-1">
                <tr className="label text-fg-3">
                  {["Calibration", "Data", "Status", "Outcome", "Test score", "Created"].map(
                    (label) => (
                      <th key={label} className="border-b border-line px-3 py-2 font-medium">
                        {label}
                      </th>
                    ),
                  )}
                </tr>
              </thead>
              <tbody>
                {calibrations.data?.calibrations.length === 0 ? (
                  <tr>
                    <td colSpan={6} className="p-8 text-center text-sm text-fg-3">
                      No calibrations yet. Import the fabricated sessions to exercise the pipeline
                      offline.
                    </td>
                  </tr>
                ) : null}
                {calibrations.data?.calibrations.map((item) => (
                  <tr
                    key={item.id}
                    tabIndex={0}
                    className="cursor-pointer border-b border-line-subtle hover:bg-surface-hover"
                    onClick={() => void navigate(`/calibration/${item.id}`)}
                    onKeyDown={(event) => {
                      if (event.key === "Enter") void navigate(`/calibration/${item.id}`);
                    }}
                  >
                    <td className="px-3 py-2 text-fg">
                      <div>{item.title}</div>
                      <div className="num text-xs text-fg-3">{item.id}</div>
                    </td>
                    <td className="label px-3 py-2 text-xs text-fg-2">{item.observation_kind}</td>
                    <td className="px-3 py-2">
                      <StatusIndicator spec={RUN_STATUS[item.status]} />
                      {item.status === "running" && item.progress ? (
                        <span className="num ml-2 text-xs text-fg-3">
                          {item.progress.phase} {item.progress.completed}/{item.progress.total}
                        </span>
                      ) : null}
                      {item.error_code ? (
                        <span className="num ml-2 text-xs text-neg">{item.error_code}</span>
                      ) : null}
                    </td>
                    <td className="label px-3 py-2 text-xs text-fg">{outcomeText(item)}</td>
                    <td className="num px-3 py-2 text-xs text-fg-2">
                      {item.headline
                        ? `${formatScales(item.headline.test_score_ppm)} vs ${formatScales(
                            item.headline.baseline_test_score_ppm,
                          )}`
                        : ""}
                    </td>
                    <td className="num px-3 py-2 text-xs text-fg-3">{item.created_at}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          )}
        </InstrumentPanel>
      </div>
    </Page>
  );
}
