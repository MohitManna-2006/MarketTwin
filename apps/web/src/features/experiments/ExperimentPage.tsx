import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { ChevronDown, ChevronUp } from "lucide-react";
import { useMemo, useState } from "react";
import { useNavigate, useParams } from "react-router";

import {
  ConfidenceIntervalPlot,
  type IntervalRow,
} from "@/components/charts/ConfidenceIntervalPlot";
import { Histogram } from "@/components/charts/Histogram";
import { PairedScatter } from "@/components/charts/PairedScatter";
import { ContextBar } from "@/components/shell/ContextBar";
import { Page } from "@/components/shell/TerminalShell";
import { DataTable, useArrayRows, type DataColumn } from "@/components/tables/DataTable";
import { Button } from "@/components/ui/Button";
import { Digest } from "@/components/ui/Digest";
import { InstrumentPanel } from "@/components/ui/InstrumentPanel";
import { MetricReadout } from "@/components/ui/MetricReadout";
import { SegmentedControl } from "@/components/ui/SegmentedControl";
import { EmptyState, ErrorState, LoadingState } from "@/components/ui/States";
import { EXPERIMENT_STATUS, StatusIndicator } from "@/components/ui/StatusIndicator";
import { ApiError, post } from "@/lib/api/client";
import { experimentQuery, experimentResultsQuery, pairedValuesQuery } from "@/lib/api/queries";
import type {
  ExperimentDetail,
  ExperimentResultsDocument,
  ExperimentSummary,
  MetricComparison,
  PairedValuesDocument,
  StatSummary,
} from "@/lib/api/types";
import {
  UNIT_LABELS,
  axisCompact,
  axisNumber,
  formatInteger,
  formatMetricValue,
  formatMilliOf,
  formatProcessing,
  unitDivisor,
} from "@/lib/formatting";

import { HEADLINE_METRICS, againstZero, metricLabel } from "./metrics";
import { Methodology } from "./Methodology";

const BASELINE = "var(--chart-baseline)";
const CANDIDATE = "var(--chart-inventory-aware)";
const DIFFERENCE = "var(--text-primary)";

function intervalText(unit: string, interval: [number, number] | null | undefined): string {
  if (!interval) return "no interval";
  return `[${formatMilliOf(unit, interval[0], { signed: true })}, ${formatMilliOf(unit, interval[1], { signed: true })}]`;
}

function plotValue(unit: string, milli: number): number {
  return milli / 1000 / unitDivisor(unit);
}

function axisFor(_unit: string, span: number): (value: number) => string {
  if (span >= 10_000) return axisCompact;
  const decimals = span >= 50 ? 0 : span >= 5 ? 1 : span >= 0.5 ? 2 : 3;
  return (value) => axisNumber(value, decimals);
}

function Running({ experiment }: { experiment: ExperimentDetail }) {
  const client = useQueryClient();
  const cancel = useMutation({
    mutationFn: () => post<ExperimentSummary>(`/experiments/${experiment.id}/cancel`),
    onSuccess: () => void client.invalidateQueries({ queryKey: ["experiments", experiment.id] }),
  });
  const progress = experiment.progress;
  const done = experiment.completed_runs + experiment.failed_runs;
  return (
    <div className="hairline-grid grid h-full grid-cols-1 lg:grid-cols-[minmax(0,1fr)_420px]">
      <InstrumentPanel
        index="01"
        title={experiment.status === "pending" ? "Queued" : "Running"}
        subtitle="counts are observed, run by run"
        actions={
          <Button
            size="sm"
            variant="danger"
            disabled={cancel.isPending || progress?.cancel_requested === true}
            onClick={() => cancel.mutate()}
          >
            {progress?.cancel_requested ? "Cancel requested" : "Cancel experiment"}
          </Button>
        }
      >
        <div className="flex h-full flex-col justify-center gap-5 px-8">
          <div className="flex flex-wrap gap-x-12 gap-y-4">
            <MetricReadout
              label="Runs completed"
              value={formatInteger(experiment.completed_runs)}
              unit={`of ${formatInteger(experiment.total_runs)}`}
              size="lg"
            />
            <MetricReadout
              label="Runs failed"
              value={formatInteger(experiment.failed_runs)}
              size="lg"
            />
            <MetricReadout
              label="Runs remaining"
              value={formatInteger(experiment.total_runs - done)}
              size="lg"
            />
          </div>
          <div
            role="progressbar"
            aria-label="Runs finished"
            aria-valuemin={0}
            aria-valuemax={experiment.total_runs}
            aria-valuenow={done}
            className="flex h-2 max-w-[720px] border border-line-strong bg-canvas"
          >
            <span
              className="bg-accent"
              style={{ width: `${(experiment.completed_runs / experiment.total_runs) * 100}%` }}
            />
            <span
              className="bg-neg-mark"
              style={{ width: `${(experiment.failed_runs / experiment.total_runs) * 100}%` }}
            />
          </div>
          <p className="max-w-[72ch] text-sm leading-snug text-fg-3">
            Cells run one at a time, seed by seed, every variant on each seed. The bar counts runs
            that have finished; nothing inside a run is observable, and no time remaining is
            estimated.{" "}
            {progress?.current_run_id ? (
              <>
                Now running <span className="num text-fg-2">{progress.current_run_id}</span>.
              </>
            ) : null}
          </p>
          {cancel.isError ? (
            <p role="alert" className="text-sm text-neg">
              {cancel.error instanceof ApiError
                ? cancel.error.message
                : "The research API did not respond."}
            </p>
          ) : null}
        </div>
      </InstrumentPanel>
      <InstrumentPanel index="02" title="Design" bodyClassName="overflow-auto">
        <Methodology experiment={experiment} results={null} />
      </InstrumentPanel>
    </div>
  );
}

interface Selection {
  metric: MetricComparison;
  values: PairedValuesDocument["metrics"][number] | undefined;
}

function Intervals({
  metric,
  baseline,
  candidate,
}: {
  metric: MetricComparison;
  baseline: string;
  candidate: string;
}) {
  const unit = metric.unit;
  const difference = metric.difference;
  const display = UNIT_LABELS[unit] ?? unit;
  if (!difference) {
    return (
      <EmptyState title="No paired difference">
        No seed has this metric defined in both variants, so there is nothing to compare.{" "}
        {formatInteger(metric.pairs_missing)} completed pairs lacked it.
      </EmptyState>
    );
  }
  const row = (
    key: string,
    label: string,
    method: string,
    summary: StatSummary,
    interval: [number, number] | null,
    color: string,
    signed: boolean,
    emphasis = true,
  ): IntervalRow => ({
    key,
    label,
    method,
    mean: plotValue(unit, summary.mean_milli),
    interval: interval ? [plotValue(unit, interval[0]), plotValue(unit, interval[1])] : null,
    meanText: formatMilliOf(unit, summary.mean_milli, { signed }),
    intervalText: interval
      ? `[${formatMilliOf(unit, interval[0], { signed })}, ${formatMilliOf(unit, interval[1], { signed })}]`
      : "no interval",
    color,
    emphasis,
  });
  const differences: IntervalRow[] = [
    row(
      "t",
      "Mean paired difference",
      "Student t · 95%",
      difference,
      difference.mean_t_interval_milli,
      DIFFERENCE,
      true,
    ),
    row(
      "boot",
      "Mean paired difference",
      "Percentile bootstrap · 95%",
      difference,
      difference.mean_bootstrap_interval_milli,
      DIFFERENCE,
      true,
      false,
    ),
  ];
  const levels: IntervalRow[] = [];
  if (metric.baseline) {
    levels.push(
      row(
        "baseline",
        baseline,
        "Mean · Student t 95%",
        metric.baseline,
        metric.baseline.mean_t_interval_milli,
        BASELINE,
        false,
      ),
    );
  }
  if (metric.candidate) {
    levels.push(
      row(
        "candidate",
        candidate,
        "Mean · Student t 95%",
        metric.candidate,
        metric.candidate.mean_t_interval_milli,
        CANDIDATE,
        false,
      ),
    );
  }
  const spanOf = (rows: IntervalRow[]) => {
    const all = rows.flatMap((item) => [...(item.interval ?? []), item.mean ?? 0]);
    return Math.max(...all) - Math.min(...all);
  };
  return (
    <div className="flex flex-col">
      <ConfidenceIntervalPlot
        rows={differences}
        zero
        format={axisFor(unit, spanOf(differences))}
        unit={`difference · ${display}`}
        label={`Mean paired difference in ${metricLabel(metric.name)} with its 95 percent intervals, against zero`}
      />
      <div className="border-t border-line-subtle" />
      <ConfidenceIntervalPlot
        rows={levels}
        format={axisFor(unit, spanOf(levels))}
        unit={`level · ${display}`}
        label={`Mean ${metricLabel(metric.name)} of each variant with its 95 percent interval`}
      />
      <dl className="grid grid-cols-5 border-t border-line-subtle">
        {(
          [
            ["Pairs", formatInteger(metric.pairs)],
            ["Median diff", formatMilliOf(unit, difference.median_milli, { signed: true })],
            ["SD of diffs", formatMilliOf(unit, difference.standard_deviation_milli)],
            ["Min diff", formatMetricValue(unit, difference.minimum, { signed: true })],
            ["Max diff", formatMetricValue(unit, difference.maximum, { signed: true })],
          ] as const
        ).map(([label, value]) => (
          <div key={label} className="border-r border-line-subtle px-3 py-1.5 last:border-r-0">
            <dt className="label text-fg-3">{label}</dt>
            <dd className="num text-sm text-fg">{value}</dd>
          </div>
        ))}
      </dl>
    </div>
  );
}

function Results({
  experiment,
  results,
  values,
}: {
  experiment: ExperimentDetail;
  results: ExperimentResultsDocument;
  values: PairedValuesDocument | undefined;
}) {
  const navigate = useNavigate();
  const comparison = results.results.comparison;
  const metrics = useMemo(() => comparison?.metrics ?? [], [comparison]);
  const [selectedName, setSelectedName] = useState("total_pnl");
  const [selectedSeed, setSelectedSeed] = useState<number | null>(null);
  const [seedView, setSeedView] = useState<"scatter" | "table">("scatter");
  const [methodology, setMethodology] = useState(false);
  const rows = useArrayRows(metrics);

  const selection: Selection | null = useMemo(() => {
    const metric = metrics.find((item) => item.name === selectedName) ?? metrics[0];
    if (!metric) return null;
    return { metric, values: values?.metrics.find((item) => item.name === metric.name) };
  }, [metrics, selectedName, values]);

  const columns = useMemo<DataColumn<MetricComparison>[]>(
    () => [
      {
        id: "metric",
        header: "Metric",
        width: "minmax(150px, 1.4fr)",
        sortValue: (metric) => metric.name,
        cell: (metric) => (
          <span className="flex items-baseline gap-2">
            <span className="label w-[50px] shrink-0 text-fg-3">{metric.scope}</span>
            <span className="truncate text-fg">{metricLabel(metric.name)}</span>
          </span>
        ),
      },
      {
        id: "unit",
        header: "Unit",
        width: "54px",
        cell: (metric) => (
          <span className="text-xs text-fg-3">{UNIT_LABELS[metric.unit] ?? metric.unit}</span>
        ),
      },
      {
        id: "pairs",
        header: "Pairs",
        title: "Seeds where both variants completed and defined this metric",
        align: "right",
        width: "58px",
        cell: (metric) => (
          <span className="num text-fg-2">
            {formatInteger(metric.pairs)}
            {metric.pairs_missing > 0 ? (
              <span className="text-warn"> −{metric.pairs_missing}</span>
            ) : null}
          </span>
        ),
      },
      {
        id: "baseline",
        header: "Baseline",
        title: "Mean over paired seeds, baseline variant",
        align: "right",
        width: "minmax(94px, 1fr)",
        cell: (metric) => (
          <span className="num text-fg-2">
            {formatMilliOf(metric.unit, metric.baseline?.mean_milli)}
          </span>
        ),
      },
      {
        id: "candidate",
        header: "Candidate",
        title: "Mean over paired seeds, candidate variant",
        align: "right",
        width: "minmax(96px, 1fr)",
        cell: (metric) => (
          <span className="num text-fg-2">
            {formatMilliOf(metric.unit, metric.candidate?.mean_milli)}
          </span>
        ),
      },
      {
        id: "difference",
        header: "Mean diff",
        title: "Mean over paired seeds of candidate minus baseline",
        align: "right",
        width: "minmax(96px, 1fr)",
        cell: (metric) => (
          <span className="num font-medium text-fg">
            {formatMilliOf(metric.unit, metric.difference?.mean_milli, { signed: true })}
          </span>
        ),
      },
      {
        id: "interval",
        header: "95% t interval of diff",
        title: "Two-sided Student t interval for the mean paired difference",
        align: "right",
        width: "minmax(178px, 1.4fr)",
        cell: (metric) => (
          <span className="num text-fg-2">
            {intervalText(metric.unit, metric.difference?.mean_t_interval_milli)}
          </span>
        ),
      },
      {
        id: "zero",
        header: "vs zero",
        title:
          "Where the t interval sits relative to zero. Not a verdict, and not corrected for the number of metrics tested.",
        width: "92px",
        cell: (metric) => (
          <span className="text-xs text-fg-2">{againstZero(metric.difference)}</span>
        ),
      },
    ],
    [],
  );

  if (!comparison) {
    return (
      <div className="hairline-grid grid h-full grid-cols-1">
        <InstrumentPanel index="01" title="Runs" subtitle="this experiment defined no comparison">
          <EmptyState title="No paired comparison">
            The experiment ran {formatInteger(results.results.design.total_runs)} runs across{" "}
            {results.results.design.variants} variants but named no baseline and candidate, so the
            core computed no paired statistics. Each run&apos;s digests are recorded in
            experiment.json.
          </EmptyState>
        </InstrumentPanel>
      </div>
    );
  }

  const metric = selection?.metric;
  const paired = selection?.values;
  const divisor = metric ? unitDivisor(metric.unit) : 1;
  const points = paired
    ? (values?.seeds ?? []).flatMap((seed, index) => {
        const baseline = paired.baseline[index];
        const candidate = paired.candidate[index];
        return baseline === null ||
          baseline === undefined ||
          candidate === null ||
          candidate === undefined
          ? []
          : [
              {
                seed,
                baseline: baseline / divisor,
                candidate: candidate / divisor,
                raw: [baseline, candidate] as const,
              },
            ];
      })
    : [];
  const span = points.length
    ? Math.max(...points.flatMap((point) => [point.baseline, point.candidate])) -
      Math.min(...points.flatMap((point) => [point.baseline, point.candidate]))
    : 1;
  const differences = points.map((point) => point.candidate - point.baseline);
  const differenceSpan = differences.length
    ? Math.max(...differences, 0) - Math.min(...differences, 0)
    : 1;
  const failed = results.results.counts.reduce((sum, item) => sum + item.failed, 0);
  const unitLabel = metric ? (UNIT_LABELS[metric.unit] ?? metric.unit) : "";

  return (
    <div className="hairline-grid grid grid-cols-1 lg:h-full lg:grid-cols-[minmax(0,1fr)_minmax(480px,39%)] lg:grid-rows-[auto_minmax(0,1fr)] 2xl:grid-cols-[minmax(0,1fr)_minmax(560px,44%)]">
      <section
        aria-label="Design"
        className="col-span-full flex flex-wrap items-stretch bg-surface-0"
      >
        {(
          [
            ["Seeds", formatInteger(results.results.design.seeds), "paired by seed"],
            [
              "Variants",
              formatInteger(results.results.design.variants),
              experiment.variants.join(" · "),
            ],
            [
              "Runs completed",
              formatInteger(experiment.completed_runs),
              `of ${formatInteger(experiment.total_runs)}`,
            ],
            [
              "Runs failed",
              formatInteger(failed),
              failed > 0 ? "counted, never filled in" : "none",
            ],
            ["Paired seeds", formatInteger(comparison.pairs_completed), "both variants completed"],
            ["Subject", `agent ${comparison.subject_agent_id}`, "the participant that is replaced"],
            ["Processing", formatProcessing(experiment.processing_ms), "wall clock"],
          ] as const
        ).map(([label, value, note]) => (
          <div
            key={label}
            className="min-w-[120px] flex-1 border-r border-line-subtle px-4 py-2 last:border-r-0"
          >
            <MetricReadout label={label} value={value} note={note} />
          </div>
        ))}
      </section>

      <div className="flex min-h-0 flex-col max-lg:h-[640px]">
        <InstrumentPanel
          index="01"
          title="Paired comparison"
          subtitle={`${comparison.candidate} minus ${comparison.baseline} · means over ${formatInteger(comparison.pairs_completed)} paired seeds`}
          className="min-h-0 flex-1"
          actions={
            <span className="hidden items-center gap-1 2xl:flex">
              {HEADLINE_METRICS.filter((name) => metrics.some((item) => item.name === name)).map(
                (name) => (
                  <Button
                    key={name}
                    size="sm"
                    variant={name === metric?.name ? "primary" : "ghost"}
                    onClick={() => setSelectedName(name)}
                  >
                    {metricLabel(name)}
                  </Button>
                ),
              )}
            </span>
          }
          footer={
            <span>
              {metrics.length} metrics fixed in the engine before any run · no multiplicity
              correction · intervals describe variation across seeds of one stylized model
            </span>
          }
        >
          <DataTable
            label="Paired comparison of every metric"
            columns={columns}
            {...rows}
            getRowId={(item) => item.name}
            selectedId={metric?.name ?? null}
            onSelect={(item) => setSelectedName(item.name)}
          />
        </InstrumentPanel>
        <section
          className="shrink-0 border-t border-line-subtle bg-surface-0"
          style={methodology ? { height: "46%" } : undefined}
        >
          <button
            type="button"
            aria-expanded={methodology}
            onClick={() => setMethodology((open) => !open)}
            className="transition-hover flex h-7 w-full items-center gap-2 border-b border-line-subtle bg-surface-1 px-3 text-left hover:bg-surface-hover"
          >
            <span className="label text-fg">
              <span className="num font-medium tracking-normal text-accent-label">05</span>
              <span className="text-fg-3"> / </span>Methodology
            </span>
            <span className="min-w-0 truncate text-xs text-fg-3">
              {formatInteger(results.results.design.seeds)} seeds · same-seed pairing · Student t
              and percentile bootstrap, 95% · {formatInteger(failed)} failed · engine{" "}
              {results.engine_version}
            </span>
            <span className="flex-1" />
            {methodology ? (
              <ChevronDown size={13} aria-hidden />
            ) : (
              <ChevronUp size={13} aria-hidden />
            )}
          </button>
          {methodology ? (
            <div
              tabIndex={0}
              role="group"
              aria-label="Methodology"
              className="h-[calc(100%-28px)] overflow-auto"
            >
              <Methodology experiment={experiment} results={results} />
            </div>
          ) : null}
        </section>
      </div>

      <div className="hairline-grid grid min-h-0 grid-rows-[auto_minmax(0,1fr)_minmax(0,1.15fr)] max-lg:h-[980px]">
        <InstrumentPanel
          index="02"
          title="Uncertainty"
          subtitle={metric ? metricLabel(metric.name) : undefined}
          unit={unitLabel}
        >
          {metric ? (
            <Intervals
              metric={metric}
              baseline={comparison.baseline}
              candidate={comparison.candidate}
            />
          ) : null}
        </InstrumentPanel>

        <InstrumentPanel
          index="03"
          title="Distributions"
          subtitle={`${formatInteger(points.length)} paired seeds · bins are a drawing choice`}
          unit={unitLabel}
          bodyClassName="grid grid-cols-2"
        >
          {points.length === 0 ? (
            <div className="col-span-2">
              {values ? (
                <EmptyState title="No per-seed values">
                  No paired seed defines {metric ? metricLabel(metric.name) : "this metric"} in both
                  variants.
                </EmptyState>
              ) : (
                <LoadingState label="Loading per-seed values" />
              )}
            </div>
          ) : (
            <>
              <figure className="flex min-h-0 flex-col border-r border-line-subtle">
                <figcaption className="flex h-5 shrink-0 items-center gap-3 overflow-hidden px-3 text-2xs whitespace-nowrap text-fg-3">
                  <span className="label">By variant</span>
                  <span className="flex items-center gap-1">
                    <span
                      aria-hidden
                      className="inline-block h-0 w-3 border-t-2 border-dashed"
                      style={{ borderColor: BASELINE }}
                    />
                    {comparison.baseline}
                  </span>
                  <span className="flex items-center gap-1">
                    <span
                      aria-hidden
                      className="inline-block h-0.5 w-3"
                      style={{ background: CANDIDATE }}
                    />
                    {comparison.candidate}
                  </span>
                </figcaption>
                <div className="relative min-h-0 flex-1">
                  <Histogram
                    groups={[
                      {
                        key: "baseline",
                        label: comparison.baseline,
                        color: BASELINE,
                        values: points.map((point) => point.baseline),
                        dashed: true,
                      },
                      {
                        key: "candidate",
                        label: comparison.candidate,
                        color: CANDIDATE,
                        values: points.map((point) => point.candidate),
                      },
                    ]}
                    format={axisFor(metric?.unit ?? "", span)}
                    label={`Distribution of ${metric ? metricLabel(metric.name) : ""} across seeds for each variant`}
                  />
                </div>
              </figure>
              <figure className="flex min-h-0 flex-col">
                <figcaption className="flex h-5 shrink-0 items-center gap-3 overflow-hidden px-3 text-2xs whitespace-nowrap text-fg-3">
                  <span className="label">Paired differences</span>
                  <span>candidate − baseline</span>
                </figcaption>
                <div className="relative min-h-0 flex-1">
                  <Histogram
                    groups={[
                      {
                        key: "difference",
                        label: "Paired difference",
                        color: "var(--text-secondary)",
                        values: differences,
                      },
                    ]}
                    zero
                    references={
                      metric?.difference
                        ? [
                            {
                              value: plotValue(metric.unit, metric.difference.mean_milli),
                              label: "mean",
                              color: "var(--accent-primary)",
                            },
                          ]
                        : []
                    }
                    format={axisFor(metric?.unit ?? "", differenceSpan)}
                    label={`Distribution of paired differences in ${metric ? metricLabel(metric.name) : ""}, with zero marked`}
                  />
                </div>
              </figure>
            </>
          )}
        </InstrumentPanel>

        <InstrumentPanel
          index="04"
          title="Per seed"
          subtitle={selectedSeed === null ? "select a seed" : `seed ${selectedSeed} selected`}
          unit={unitLabel}
          actions={
            <>
              <SegmentedControl
                label="Per-seed view"
                value={seedView}
                onChange={setSeedView}
                options={[
                  { value: "scatter", label: "Scatter" },
                  { value: "table", label: "Table" },
                ]}
              />
              <Button
                size="sm"
                variant="primary"
                disabled={selectedSeed === null}
                onClick={() => void navigate(`/experiments/${experiment.id}/seeds/${selectedSeed}`)}
                title="Open the baseline and candidate runs of this seed side by side"
              >
                Inspect seed pair
              </Button>
            </>
          }
          bodyClassName="relative"
        >
          {points.length === 0 ? (
            <EmptyState title="No paired seeds">Nothing to plot for this metric.</EmptyState>
          ) : seedView === "scatter" ? (
            <PairedScatter
              points={points}
              selectedSeed={selectedSeed}
              onSelect={setSelectedSeed}
              format={axisFor(metric?.unit ?? "", span)}
              baselineLabel={comparison.baseline}
              candidateLabel={comparison.candidate}
              label={`Per-seed ${metric ? metricLabel(metric.name) : ""}: baseline against candidate`}
            />
          ) : (
            <SeedTable
              points={points}
              unit={metric?.unit ?? ""}
              selectedSeed={selectedSeed}
              onSelect={setSelectedSeed}
              onOpen={(seed) => void navigate(`/experiments/${experiment.id}/seeds/${seed}`)}
              baseline={comparison.baseline}
              candidate={comparison.candidate}
            />
          )}
        </InstrumentPanel>
      </div>
    </div>
  );
}

interface SeedPoint {
  seed: number;
  raw: readonly [number, number];
}

function SeedTable({
  points,
  unit,
  selectedSeed,
  onSelect,
  onOpen,
  baseline,
  candidate,
}: {
  points: SeedPoint[];
  unit: string;
  selectedSeed: number | null;
  onSelect: (seed: number) => void;
  onOpen: (seed: number) => void;
  baseline: string;
  candidate: string;
}) {
  const rows = useArrayRows(points);
  const columns = useMemo<DataColumn<SeedPoint>[]>(
    () => [
      {
        id: "seed",
        header: "Seed",
        align: "right",
        width: "72px",
        sortValue: (point) => point.seed,
        cell: (point) => <span className="num text-fg">{point.seed}</span>,
      },
      {
        id: "baseline",
        header: baseline,
        align: "right",
        width: "minmax(120px, 1fr)",
        sortValue: (point) => point.raw[0],
        cell: (point) => (
          <span className="num text-fg-2">
            {formatMetricValue(unit, point.raw[0], { signed: true })}
          </span>
        ),
      },
      {
        id: "candidate",
        header: candidate,
        align: "right",
        width: "minmax(120px, 1fr)",
        sortValue: (point) => point.raw[1],
        cell: (point) => (
          <span className="num text-fg-2">
            {formatMetricValue(unit, point.raw[1], { signed: true })}
          </span>
        ),
      },
      {
        id: "difference",
        header: "Difference",
        align: "right",
        width: "minmax(120px, 1fr)",
        sortValue: (point) => point.raw[1] - point.raw[0],
        cell: (point) => (
          <span className="num font-medium text-fg">
            {formatMetricValue(unit, point.raw[1] - point.raw[0], { signed: true })}
          </span>
        ),
      },
    ],
    [unit, baseline, candidate],
  );
  return (
    <div className="absolute inset-0">
      <DataTable
        label="Per-seed values"
        columns={columns}
        {...rows}
        sortable
        getRowId={(point) => String(point.seed)}
        selectedId={selectedSeed === null ? null : String(selectedSeed)}
        onSelect={(point) => onSelect(point.seed)}
        onActivate={(point) => onOpen(point.seed)}
      />
    </div>
  );
}

export function ExperimentPage() {
  const { experimentId = "" } = useParams();
  const navigate = useNavigate();
  const query = useQuery(experimentQuery(experimentId));
  const experiment = query.data;
  const finished = experiment !== undefined && !["pending", "running"].includes(experiment.status);
  const hasResults = finished && experiment.results_digest !== null;
  const results = useQuery(experimentResultsQuery(experimentId, hasResults));
  const values = useQuery(pairedValuesQuery(experimentId, hasResults));

  const context = (
    <ContextBar
      crumbs={[
        { label: "Experiment", to: "/experiments" },
        ...(experiment
          ? [
              { label: experiment.name, verbatim: true },
              { label: experiment.scenario_key?.replaceAll("_", " ") ?? "custom base" },
              { label: `${experiment.seed_count} seeds` },
              { label: `Engine ${experiment.engine_version}` },
            ]
          : [{ label: experimentId, verbatim: true }]),
      ]}
      status={
        experiment ? <StatusIndicator spec={EXPERIMENT_STATUS[experiment.status]} /> : undefined
      }
    >
      {experiment?.results_digest ? (
        <span className="label hidden items-baseline gap-1.5 text-fg-3 lg:flex">
          Results digest <Digest value={experiment.results_digest} label="results digest" />
        </span>
      ) : null}
      <Button size="sm" onClick={() => void navigate("/experiments/new")}>
        New experiment
      </Button>
    </ContextBar>
  );

  let body;
  if (query.isPending) body = <LoadingState label="Loading experiment" />;
  else if (query.isError)
    body = <ErrorState error={query.error} onRetry={() => void query.refetch()} />;
  else if (experiment && !finished) body = <Running experiment={experiment} />;
  else if (experiment && !hasResults) {
    body = (
      <div className="hairline-grid grid h-full grid-cols-1 lg:grid-cols-[minmax(0,1fr)_420px]">
        <InstrumentPanel
          index="01"
          title="Experiment failed"
          subtitle="no results document was published"
        >
          <div role="alert" className="flex flex-col gap-2 p-4">
            <p className="label text-fg-3">Engine error</p>
            <p className="num text-md font-medium text-neg">
              {experiment.error?.code ?? "ExperimentFailed"}
            </p>
            <p className="text-base text-fg">
              {experiment.error?.message ?? "The experiment did not finish."}
            </p>
            <p className="text-sm text-fg-3">
              The engine writes experiment.json last. Without it there are no statistics to show.
            </p>
          </div>
        </InstrumentPanel>
        <InstrumentPanel index="02" title="Design" bodyClassName="overflow-auto">
          <Methodology experiment={experiment} results={null} />
        </InstrumentPanel>
      </div>
    );
  } else if (results.isError)
    body = <ErrorState error={results.error} onRetry={() => void results.refetch()} />;
  else if (experiment && results.data) {
    body = <Results experiment={experiment} results={results.data} values={values.data} />;
  } else body = <LoadingState label="Loading results" />;

  return <Page context={context}>{body}</Page>;
}
