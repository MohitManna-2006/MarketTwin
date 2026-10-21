import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { render, screen, within } from "@testing-library/react";
import { afterEach, expect, it, vi } from "vitest";
import { createMemoryRouter, RouterProvider } from "react-router";

import type { HistoricalImport } from "@/lib/api/types";

import { CalibrationPage } from "./CalibrationPage";
import { eligibleSessions } from "./CalibrationsPage";
import { FACT_ORDER, formatFact, formatParameter, formatScales } from "./format";

afterEach(() => vi.unstubAllGlobals());

const facts = Object.fromEntries(FACT_ORDER.map((name) => [name, 1000]));
const score = (distance: number) => ({
  valid: true,
  invalid_reason: null,
  seeds: 100,
  facts: Object.fromEntries(
    FACT_ORDER.map((name) => [name, { mean: 1000, deviation: 10, distance_ppm: distance }]),
  ),
  score_ppm: Math.abs(distance),
});
const targets = {
  sessions: 2,
  reference_price_cents: 25_000,
  mean: facts,
  deviation: facts,
  scale: facts,
};
const row = {
  id: "cal-0123456789",
  spec_key: "aapl_iex",
  spec_digest: "d",
  title: "AAPL on IEX",
  symbol: "AAPL",
  feed: "iex",
  observation_kind: "OBSERVED IEX DATA",
  status: "completed",
  created_at: "2026-10-20T00:00:00.000Z",
  started_at: null,
  finished_at: null,
  sessions: [],
  progress: null,
  headline: null,
  error_code: null,
  error_message: null,
};
const report = {
  calibration: row,
  spec: {},
  units: {},
  groups: Object.fromEntries(FACT_ORDER.map((name) => [name, "spread"])),
  observed: [],
  report: {
    calibration_id: row.id,
    title: row.title,
    symbol: "AAPL",
    feed: "iex",
    observation_kind: "OBSERVED IEX DATA",
    window: { start_local: "10:30", end_local: "11:30", timezone: "America/New_York" },
    outcome: "null_result",
    label: "CALIBRATION NULL RESULT",
    null_reasons: ["only 3 of 12 facts are within two scales on test"],
    claim: "The fit did not hold out of sample; no closeness to observed facts is claimed.",
    not_claimed: ["realism of the synthetic market"],
    acceptance: {
      distance_ppm_limit: 2_000_000,
      facts_required: 9,
      facts_within: 3,
      beats_baseline: true,
    },
    deviations_from_adr: [],
    sessions: [
      {
        session_id: "hist-0000000001",
        day: "2026-10-19",
        role: "test",
        admitted: true,
        rejection_reasons: [],
      },
      {
        session_id: "hist-0000000002",
        day: "2026-10-06",
        role: "train",
        admitted: false,
        rejection_reasons: ["quotes are two-sided for less than 90% of the window"],
      },
    ],
    targets: { train: targets, validation: targets, test: targets },
    reference_price_cents: 25_012,
    stage1: {
      candidates: 257,
      valid: 250,
      invalid: 7,
      fit_ppm_quantiles: { p0: 1, p10: 2_000_000, p50: 3_000_000, p90: 9_000_000, p100: 10 },
      fit_ppm: [],
      baseline_fit: score(4_000_000),
      finalists: [3, 9],
    },
    stage2: [
      {
        candidate: 3,
        parameters: { noise_interval_ns: 500_000_000 },
        fit: score(1_000_000),
        validation: score(2_500_000),
      },
    ],
    selected: { candidate: 3, parameters: { noise_interval_ns: 500_000_000 } },
    test: { selected: score(-2_500_000), baseline: score(5_000_000) },
    sensitivity: {},
    seeds: { stage1: [1, 2], stage2: [3], test: [4] },
  },
};

it("shows a null result as prominently as a pass, with every distance", async () => {
  vi.stubGlobal(
    "fetch",
    vi.fn((input: string) =>
      Promise.resolve(
        new Response(JSON.stringify(input.endsWith("/report") ? report : row), {
          status: 200,
          headers: { "content-type": "application/json" },
        }),
      ),
    ),
  );
  const router = createMemoryRouter(
    [{ path: "/calibration/:calibrationId", element: <CalibrationPage /> }],
    { initialEntries: ["/calibration/cal-0123456789"] },
  );
  const client = new QueryClient({ defaultOptions: { queries: { retry: false } } });
  render(
    <QueryClientProvider client={client}>
      <RouterProvider router={router} />
    </QueryClientProvider>,
  );
  const outcome = await screen.findByRole("region", { name: "Outcome" });
  expect(within(outcome).getByText("CALIBRATION NULL RESULT")).toBeInTheDocument();
  expect(within(outcome).getByText("OBSERVED IEX DATA")).toBeInTheDocument();
  expect(within(outcome).getByText(/only 3 of 12 facts/)).toBeInTheDocument();
  expect(screen.getAllByText("-2.50")).toHaveLength(12);
  expect(screen.getAllByText(/outside two scales/)).toHaveLength(12);
  expect(screen.getByText("3 of 12 (needs 9)")).toBeInTheDocument();
  expect(screen.getByText(/two-sided for less than 90%/)).toBeInTheDocument();
  expect(screen.getByText("250.12 USD")).toBeInTheDocument();
});

it("formats stored integers in their stated units", () => {
  expect(formatFact("spread_cents_p50", 1500)).toBe("1.500 ¢");
  expect(formatFact("two_sided_share", 999_000)).toBe("99.90 %");
  expect(formatFact("trade_return_lag1_autocorr", -337_689)).toBe("-0.3377");
  expect(formatFact("interarrival_cv", null)).toBe("undefined");
  expect(formatScales(2_000_000)).toBe("2.00");
  expect(formatParameter("noise_interval_ns", 250_000_000)).toBe("250 ms");
  expect(formatParameter("noise_market_order_ppm", 350_000)).toBe("35.0 %");
  expect(formatParameter("maker_half_spread_ticks", undefined)).toBe("baseline");
});

it("offers only sessions with the spec's provider, symbol, and window length", () => {
  const session = (
    id: string,
    start: string,
    end: string,
    provider = "alpaca",
  ): HistoricalImport => ({
    id,
    provider,
    feed: "iex",
    symbol: "AAPL",
    start_time: start,
    end_time: end,
    status: "completed",
    created_at: "",
    started_at: null,
    finished_at: null,
    manifest: null,
    error_code: null,
    error_message: null,
  });
  const spec = {
    key: "aapl_iex",
    title: "",
    symbol: "AAPL",
    feed: "iex",
    window: { start_local: "10:30", end_local: "11:30", timezone: "America/New_York" },
    split: { train: 6, validation: 2, test: 2 },
    search: {},
    grid: {},
    grid_size: 1,
    sessions_required: 10,
    spec_sha256: "",
    deviations_from_adr: [],
    problems: [],
  };
  const eligible = eligibleSessions(spec, [
    session("b", "2026-10-19T14:30:00Z", "2026-10-19T15:30:00Z"),
    session("a", "2026-10-16T14:30:00Z", "2026-10-16T15:30:00Z"),
    session("short", "2026-10-15T14:30:00Z", "2026-10-15T14:35:00Z"),
    session("local", "2026-10-14T14:30:00Z", "2026-10-14T15:30:00Z", "local_file"),
  ]);
  expect(eligible.map((item) => item.id)).toEqual(["a", "b"]);
});
