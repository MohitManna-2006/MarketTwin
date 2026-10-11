import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, expect, it, vi } from "vitest";
import { createMemoryRouter, RouterProvider } from "react-router";

import { AIResearchPage } from "./AIResearchPage";

const plan = {
  question: "Compare makers over paired seeds.",
  hypothesis: "Inventory changes risk.",
  scenario_key: "liquidity_shock",
  scenario_digest: "abc",
  seed_count: 2,
  total_runs: 4,
  subject_agent_id: 3,
  baseline: "baseline_maker",
  candidate: "inventory_maker",
  primary_metric: "total_pnl",
  secondary_metrics: ["max_drawdown"],
  methodology: "Native paired comparison",
  limitations: [],
  config_digest: "a".repeat(64),
};
const investigation = {
  id: "inv-test",
  question: plan.question,
  status: "AWAITING_APPROVAL",
  created_at: "2026-10-11T00:00:00Z",
  plan,
  experiment_id: null,
  experiment_status: null,
  progress: null,
  report: null,
  error_code: null,
  error_message: null,
  events: [],
  messages: [],
};

function setup(detail: object) {
  const fetcher = vi.fn((input: string, init?: RequestInit) => {
    const body = input.includes("?limit=") ? { total: 1, investigations: [detail] } : detail;
    return Promise.resolve(
      new Response(JSON.stringify(body), {
        status: init?.method === "POST" ? 200 : 200,
        headers: { "content-type": "application/json" },
      }),
    );
  });
  vi.stubGlobal("fetch", fetcher);
  const router = createMemoryRouter(
    [{ path: "/ai-research/:investigationId", element: <AIResearchPage /> }],
    { initialEntries: ["/ai-research/inv-test"] },
  );
  const client = new QueryClient({ defaultOptions: { queries: { retry: false } } });
  const view = render(
    <QueryClientProvider client={client}>
      <RouterProvider router={router} />
    </QueryClientProvider>,
  );
  return { fetcher, ...view };
}

afterEach(() => vi.unstubAllGlobals());

it("shows the plan and submits its exact digest on approval", async () => {
  const { fetcher, unmount } = setup(investigation);
  expect(await screen.findByText("4 native runs")).toBeInTheDocument();
  fireEvent.click(screen.getByRole("button", { name: "Approve & run" }));
  await waitFor(() =>
    expect(fetcher).toHaveBeenCalledWith(
      "/api/v1/ai/investigations/inv-test/approve",
      expect.objectContaining({ body: JSON.stringify({ config_digest: plan.config_digest }) }),
    ),
  );
  unmount();
});

it("shows native numbers and selects their evidence reference", async () => {
  const reference = "experiment:exp-test:metric:total_pnl";
  const detail = {
    ...investigation,
    status: "REPORT_READY",
    experiment_id: "exp-test",
    report: {
      question: plan.question,
      hypothesis: plan.hypothesis,
      experiment_id: "exp-test",
      engine_version: "0.4.0",
      config_digest: "digest",
      results_digest: "results",
      pairs_completed: 2,
      interpretation: "A measured effect.",
      limitations: ["Synthetic market."],
      follow_up: ["Repeat."],
      replay_status: "not_checked",
      claims: [
        {
          metric: "total_pnl",
          scope: "subject",
          unit: "minor",
          pairs: 2,
          baseline_mean_milli: 1000,
          candidate_mean_milli: 2000,
          difference_mean_milli: 1000,
          difference_t_interval_milli: [500, 1500],
          difference_bootstrap_interval_milli: [0, 2000],
          evidence_ref: reference,
        },
      ],
    },
  };
  const { unmount } = setup(detail);
  expect(await screen.findAllByText("1 minor")).not.toHaveLength(0);
  const metricCell = screen.getAllByText("total pnl").find((node) => node.tagName === "TD");
  expect(metricCell).toBeDefined();
  if (metricCell) fireEvent.click(metricCell);
  expect(screen.getByText(reference)).toBeInTheDocument();
  unmount();
});
