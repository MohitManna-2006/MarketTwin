import { readFileSync } from "node:fs";
import { resolve } from "node:path";

import { render, screen } from "@testing-library/react";
import { describe, expect, it } from "vitest";

import type { ExperimentResultsDocument, MetricComparison } from "@/lib/api/types";
import { formatMilliOf } from "@/lib/formatting";

import { ConfidenceIntervalPlot, type IntervalRow } from "./ConfidenceIntervalPlot";

// The engine's own golden results document, pinned by the C++ test suite.
const golden = JSON.parse(
  readFileSync(
    resolve(process.cwd(), "../../core/tests/replay/golden/phase2b_experiment_results.json"),
    "utf8",
  ),
) as ExperimentResultsDocument["results"];

function rowsFor(metric: MetricComparison): IntervalRow[] {
  const difference = metric.difference;
  if (!difference) return [];
  const interval = difference.mean_t_interval_milli;
  return [
    {
      key: "t",
      label: "Mean paired difference",
      method: "Student t · 95%",
      mean: difference.mean_milli / 1000,
      interval: interval ? [interval[0] / 1000, interval[1] / 1000] : null,
      meanText: formatMilliOf(metric.unit, difference.mean_milli, { signed: true }),
      intervalText: interval
        ? `[${formatMilliOf(metric.unit, interval[0], { signed: true })}, ${formatMilliOf(metric.unit, interval[1], { signed: true })}]`
        : "no interval",
      color: "var(--text-primary)",
    },
  ];
}

describe("ConfidenceIntervalPlot", () => {
  const metric = golden.comparison?.metrics.find((item) => item.name === "total_pnl");

  it("prints the engine's mean and interval, digit for digit", () => {
    expect(metric?.difference).toMatchObject({
      mean_milli: 15333,
      mean_t_interval_milli: [-50641, 81307],
    });
    render(
      <ConfidenceIntervalPlot
        rows={rowsFor(metric as MetricComparison)}
        zero
        format={(value) => value.toFixed(0)}
        unit="difference · minor"
        label="Mean paired difference in total P&L"
      />,
    );
    expect(screen.getByText("+15.3")).toBeInTheDocument();
    expect(screen.getByText("[−50.6, +81.3]")).toBeInTheDocument();
    expect(screen.getByText("STUDENT T · 95%")).toBeInTheDocument();
  });

  it("draws an interval that contains zero no differently from one that does not", () => {
    const { container } = render(
      <ConfidenceIntervalPlot
        rows={[
          { ...(rowsFor(metric as MetricComparison)[0] as IntervalRow), key: "crosses" },
          {
            key: "clear",
            label: "Another metric",
            method: "Student t · 95%",
            mean: 40,
            interval: [20, 60],
            meanText: "+40.0",
            intervalText: "[+20.0, +60.0]",
            color: "var(--text-primary)",
          },
        ]}
        zero
        format={(value) => value.toFixed(0)}
        unit="minor"
        label="Two intervals"
      />,
    );
    const strokes = [...container.querySelectorAll("g[stroke]")].map((group) => [
      group.getAttribute("stroke"),
      group.getAttribute("stroke-width"),
    ]);
    expect(strokes).toHaveLength(2);
    expect(strokes[0]).toEqual(strokes[1]);
  });

  it("has an accessible name and draws nothing for a metric with no interval", () => {
    render(
      <ConfidenceIntervalPlot
        rows={[
          {
            key: "single",
            label: "Mean paired difference",
            method: "Student t · 95%",
            mean: 3,
            interval: null,
            meanText: "+3.0",
            intervalText: "no interval",
            color: "var(--text-primary)",
          },
        ]}
        zero
        format={String}
        unit="minor"
        label="One pair has a mean and no interval"
      />,
    );
    expect(
      screen.getByRole("img", { name: "One pair has a mean and no interval" }),
    ).toBeInTheDocument();
    expect(screen.getByText("no interval")).toBeInTheDocument();
  });
});
