import AxeBuilder from "@axe-core/playwright";
import { expect, test, type Page } from "@playwright/test";

import { formatInteger, formatMilliOf, formatTwiceTicks } from "../src/lib/formatting/numbers";

// One researcher's session, in order, against the real engine. Each step depends on what
// the step before it produced, as it would at a desk.
test.describe.configure({ mode: "serial" });

const API = "/api/v1";
let runId = "";
let experimentId = "";
let pairPath = "";

/** The value cell of a label-value line inside a named panel. */
function line(page: Page, panel: string, label: string) {
  return page
    .locator("section", { has: page.getByRole("heading", { name: new RegExp(panel, "i") }) })
    .locator("dt", { has: page.locator(`span:text-is("${label}")`) })
    .locator("xpath=following-sibling::dd");
}

test("an empty workspace explains itself and shows no invented data", async ({ page }) => {
  await page.goto("/");
  await expect(page.getByText("API connected")).toBeVisible();
  await expect(page.getByText("This workspace has no runs yet")).toBeVisible();
  await expect(page.getByText("No experiments stored")).toBeVisible();
  // Scenarios are the shipped configs, read from the API.
  await expect(page.getByRole("button", { name: /Liquidity withdrawal/ })).toBeVisible();
  await expect(page.locator("canvas")).toHaveCount(0);
});

test("a simulation is configured, validated by the engine, and run", async ({ page }) => {
  await page.goto("/");
  await page.getByRole("button", { name: "Configure the baseline run" }).click();
  await expect(page).toHaveURL(/\/simulations\/new\?scenario=baseline/);
  await expect(page.getByText("Config valid").first()).toBeVisible();
  await expect(page.getByRole("grid", { name: "Agent population" }).getByRole("row")).toHaveCount(
    12,
  );

  // A field-level error appears at the field, and the run is blocked.
  const reference = page.getByLabel(/reference price/i);
  await reference.fill("0");
  await expect(page.getByText("At least 1")).toBeVisible();
  await expect(page.getByText("Config invalid").first()).toBeVisible();
  await expect(page.getByRole("button", { name: "Run simulation" })).toBeDisabled();
  await reference.fill("15000");

  // A rule relating two fields is the engine's, and its message is shown verbatim.
  await page
    .getByRole("grid", { name: "Agent population" })
    .getByText("Inventory-aware maker")
    .click();
  const levels = page.getByLabel(/^levels/i);
  await levels.fill("9");
  await expect(
    page.getByText("maker levels must fit twice within max_open_orders").first(),
  ).toBeVisible();
  await levels.fill("3");

  await page.getByLabel("Seed", { exact: true }).fill("7");
  await page.getByLabel("Run name").fill("e2e-baseline");
  await expect(page.getByText("Config valid").first()).toBeVisible();
  await page.getByRole("button", { name: "Run simulation" }).click();

  await expect(page).toHaveURL(/\/runs\/run-[0-9a-f]+/);
  runId = /\/runs\/(run-[0-9a-f]+)/.exec(page.url())?.[1] ?? "";
  await expect(page.getByRole("heading", { name: /Market price/ })).toBeVisible({
    timeout: 30_000,
  });
  await expect(page.getByText("Completed", { exact: true })).toBeVisible();
  await expect(page.getByRole("navigation", { name: "Context" })).toContainText("Seed 7");
  await expect(page.getByRole("navigation", { name: "Context" })).toContainText("Engine 0.4.0");
});

test("the timeline moves every panel to the same recorded sample", async ({ page, request }) => {
  await page.goto(`/runs/${runId}`);
  await expect(page.getByRole("heading", { name: /Order book/ })).toBeVisible();
  const slider = page.getByRole("slider");
  await expect(slider).toHaveAttribute("aria-valuetext", /sample 3600 of 3600/);

  // Move to sample 1,800 and compare each panel with what the API recorded there.
  await slider.fill("1799");
  const window = await (
    await request.get(`${API}/runs/${runId}/samples?start=1799&count=1&agents=all`)
  ).json();
  const columns = window.columns as Record<string, (number | null)[]>;
  const book = window.books[0] as { timestamp_ns: number; bids: number[][]; asks: number[][] };
  expect(columns.timestamp_ns[0]).toBe(1_800_000_000_000);
  expect(book.timestamp_ns).toBe(1_800_000_000_000);

  await expect(page.getByText("00:30:00.000").first()).toBeVisible();
  await expect(line(page, "Market telemetry", "Trades")).toContainText(
    formatInteger(columns.trade_count[0]),
  );
  await expect(line(page, "Market telemetry", "Volume")).toContainText(
    formatInteger(columns.volume[0]),
  );
  await expect(line(page, "Market telemetry", "Mark")).toContainText(
    formatTwiceTicks(columns.mark_twice_ticks[0], { fixed: true }),
  );
  const ladder = page.getByRole("table", { name: "Order book depth at the cursor" });
  await expect(
    ladder.getByRole("rowgroup", { name: "Bids" }).getByRole("row").first(),
  ).toContainText(formatInteger(book.bids[0]?.[0]));
  await expect(
    ladder.getByRole("rowgroup", { name: "Asks" }).getByRole("row").last(),
  ).toContainText(formatInteger(book.asks[0]?.[0]));
  await expect(ladder).toContainText("00:30:00.000");
  const agent = window.agents["3"] as Record<string, number[]>;
  await expect(
    page.getByRole("grid", { name: "Agent accounts at the cursor" }).getByRole("row").nth(3),
  ).toContainText(formatInteger(agent.equity_minor[0], { signed: true }));

  // Keys step one recorded sample at a time, and Home and End reach the ends.
  await page.locator("#workspace").focus();
  await page.keyboard.press("ArrowRight");
  await expect(slider).toHaveAttribute("aria-valuetext", /sample 1801 of 3600/);
  await page.keyboard.press("Home");
  await expect(slider).toHaveAttribute("aria-valuetext", /00:00:01.000 elapsed, sample 1 of 3600/);
  await page.keyboard.press("End");
  await expect(slider).toHaveAttribute(
    "aria-valuetext",
    /01:00:00.000 elapsed, sample 3600 of 3600/,
  );

  // Playback advances through samples and can be paused.
  await page.keyboard.press("Home");
  await page.keyboard.press("Space");
  await expect(slider).not.toHaveAttribute("aria-valuetext", /sample 1 of/, { timeout: 5_000 });
  await page.keyboard.press("Space");

  // Selecting an agent links the stream and the chart to it.
  await page
    .getByRole("grid", { name: "Agent accounts at the cursor" })
    .getByRole("row")
    .nth(3)
    .click();
  await expect(page).toHaveURL(/agent=3/);
  await expect(page.getByText("Agent 3 inventory")).toBeVisible();
  await expect(page.getByText("Agent 3", { exact: true })).toBeVisible();
});

test("an agent's figures are the engine's accounting", async ({ page, request }) => {
  const run = await (await request.get(`${API}/runs/${runId}`)).json();
  const report = run.agents.find((agent: { id: number }) => agent.id === 3).report as Record<
    string,
    number
  >;
  await page.goto(`/runs/${runId}/agents/3`);
  await expect(page.getByRole("heading", { name: /Agent 3 over the run/ })).toBeVisible();
  const summary = page.locator("table", { has: page.getByText("End of run · from metrics.json") });
  await expect(summary.getByRole("row", { name: /Total P&L/ })).toContainText(
    formatInteger(report.total_pnl_minor, { signed: true }),
  );
  await expect(summary.getByRole("row", { name: /Max drawdown/ })).toContainText(
    formatInteger(report.max_drawdown_minor),
  );
  await expect(
    page.getByRole("grid", { name: "Ledger entries of agent 3" }).getByRole("row").nth(1),
  ).toBeVisible();
  // Comparison with another agent of the same run.
  await page.getByRole("button", { name: "Compare with agent 1", exact: true }).click();
  await expect(page).toHaveURL(/compare=1/);
  await expect(page.getByText(/same run, same market history/)).toBeVisible();
});

test("replay verification is requested and its result is shown", async ({ page }) => {
  await page.goto(`/runs/${runId}/replay`);
  await expect(page.getByText("This run has not been verified")).toBeVisible();
  await page.getByRole("button", { name: "Verify replay" }).click();
  await expect(page.getByText("The recording reproduces exactly")).toBeVisible({ timeout: 30_000 });
  await expect(page.getByText("Replay verified").first()).toBeVisible();
  await expect(page.getByText("matched").first()).toBeVisible();
  // The recorded events are searchable.
  const records = page.getByRole("grid", { name: "Recorded events" });
  await expect(records.getByRole("row").nth(1)).toContainText("Order Arrival");
  await page.getByPlaceholder("Go to sequence").fill("4281");
  await expect(records.getByRole("row", { name: /4,281/ })).toBeVisible();
});

test("a stored run is listed on the overview and can be reopened", async ({ page }) => {
  await page.goto("/");
  const row = page
    .getByRole("grid", { name: "Recent simulations" })
    .getByRole("row", { name: /e2e-baseline/ });
  await expect(row).toContainText("8,");
  await expect(row).toContainText("Verified");
  await row.click();
  await expect(page).toHaveURL(new RegExp(`/runs/${runId}`));
  await expect(page.getByRole("heading", { name: /Market price/ })).toBeVisible();
});

test("a paired experiment runs and reports the engine's statistics", async ({ page, request }) => {
  await page.goto("/experiments/new");
  await expect(page.getByText("Design valid").first()).toBeVisible();
  await page.getByLabel("Experiment name").fill("e2e_makers");
  await page.getByLabel("Count").fill("3");
  await page.getByRole("checkbox", { name: "Override" }).check();
  await page.getByLabel("Simulated horizon per run", { exact: true }).fill("2");
  await page.getByLabel("Simulated horizon per run unit").selectOption({ label: "min" });
  await expect(page.getByText("3 seeds × 2 variants =")).toBeVisible();
  await expect(page.getByText("Design valid").first()).toBeVisible();
  await page.getByRole("button", { name: "Run experiment" }).click();

  await expect(page).toHaveURL(/\/experiments\/exp-[0-9a-f]+/);
  experimentId = /\/experiments\/(exp-[0-9a-f]+)/.exec(page.url())?.[1] ?? "";
  // While it runs, the page counts finished runs and estimates nothing.
  await expect(page.getByRole("grid", { name: "Paired comparison of every metric" })).toBeVisible({
    timeout: 90_000,
  });

  const results = (await (await request.get(`${API}/experiments/${experimentId}/results`)).json())
    .document.results;
  const metric = results.comparison.metrics.find(
    (item: { name: string }) => item.name === "total_pnl",
  );
  const row = page
    .getByRole("grid", { name: "Paired comparison of every metric" })
    .getByRole("row", { name: /Total P&L/ });
  await expect(row).toContainText(
    formatMilliOf("minor", metric.difference.mean_milli, { signed: true }),
  );
  const [low, high] = metric.difference.mean_t_interval_milli as [number, number];
  await expect(row).toContainText(
    `[${formatMilliOf("minor", low, { signed: true })}, ${formatMilliOf("minor", high, { signed: true })}]`,
  );
  await expect(
    page.getByRole("img", { name: /Mean paired difference in Total P&L/ }),
  ).toBeVisible();
  await expect(page.getByText("Runs failed").locator("..")).toContainText("0");

  // Methodology is collapsible and states its limits.
  await page.getByRole("button", { name: /Methodology/ }).click();
  await expect(page.getByText(/no\s+multiplicity correction/).first()).toBeVisible();
});

test("one seed's two runs are regenerated, digest-checked, and shown side by side", async ({
  page,
}) => {
  await page.goto(`/experiments/${experimentId}`);
  await page.getByRole("radio", { name: "Table" }).click();
  await page.getByRole("grid", { name: "Per-seed values" }).getByRole("row").nth(2).click();
  await page.getByRole("button", { name: "Inspect seed pair" }).click();
  await expect(page).toHaveURL(new RegExp(`/experiments/${experimentId}/seeds/\\d+`));
  await page.getByRole("button", { name: "Regenerate and inspect" }).click();
  await expect(page.getByText("Digests match the experiment")).toHaveCount(2, { timeout: 60_000 });
  await expect(page.getByText("Linked by elapsed simulation time")).toBeVisible();
  pairPath = new URL(page.url()).pathname;
  await expect(page.getByRole("link", { name: "Open in microscope" })).toHaveCount(2);
});

test("every screen passes an automated WCAG 2.1 A and AA audit", async ({ page }) => {
  const screens = [
    "/",
    "/simulations",
    "/simulations/new?scenario=liquidity_shock",
    `/runs/${runId}`,
    `/runs/${runId}/agents/3?compare=1`,
    `/runs/${runId}/replay`,
    "/experiments",
    "/experiments/new",
    `/experiments/${experimentId}`,
    pairPath,
    "/replay",
  ];
  const findings: string[] = [];
  for (const path of screens) {
    await page.goto(path);
    await page.waitForLoadState("networkidle");
    const { violations } = await new AxeBuilder({ page })
      .withTags(["wcag2a", "wcag2aa", "wcag21aa"])
      .analyze();
    for (const violation of violations) {
      findings.push(
        `${path} · ${violation.id} (${violation.impact}): ${violation.nodes.length} × ${violation.nodes[0]?.target.join(" ")}`,
      );
    }
  }
  expect(findings).toEqual([]);
});

test("when the API is unreachable the terminal says so and shows nothing stale", async ({
  page,
}) => {
  await page.route("**/api/v1/**", (route) => route.abort());
  await page.goto("/");
  await expect(page.getByText("API unreachable", { exact: true })).toBeVisible();
  await expect(page.getByText("RESEARCH API UNREACHABLE").first()).toBeVisible();
  await expect(page.getByText("e2e-baseline")).toHaveCount(0);
  await page.goto(`/runs/${runId}`);
  await expect(page.getByText("RESEARCH API UNREACHABLE").first()).toBeVisible();
  await expect(page.locator("canvas")).toHaveCount(0);
});

test("AI research requires approval and reports native evidence", async ({ page, request }) => {
  await page.goto("/ai-research");
  await expect(page.getByRole("heading", { name: "AI research" })).toBeVisible();
  await page
    .getByLabel("RESEARCH QUESTION")
    .fill(
      "[E2E fixture] Compare makers in liquidity withdrawal over two paired seeds; analyze P&L and inventory.",
    );
  await page.getByRole("button", { name: "Build research plan" }).click();
  await expect(page).toHaveURL(/\/ai-research\/inv-[0-9a-f]+/);
  await expect(page.getByText("4 native runs")).toBeVisible();
  const investigationId = /\/ai-research\/(inv-[0-9a-f]+)/.exec(page.url())?.[1] ?? "";
  const before = await request.get(`${API}/ai/investigations/${investigationId}`);
  expect(before.ok()).toBeTruthy();
  expect((await before.json()).experiment_id).toBeNull();
  await page.getByRole("button", { name: "Approve & run" }).click();
  await expect(page.getByText("INSTITUTIONAL RESEARCH NOTE")).toBeVisible({ timeout: 60_000 });
  const after = await request.get(`${API}/ai/investigations/${investigationId}`);
  const investigation = await after.json();
  expect(investigation.status).toBe("REPORT_READY");
  const report = investigation.report;
  const native = await request.get(`${API}/experiments/${report.experiment_id}/results`);
  const document = (await native.json()).document;
  expect(report.results_digest).toBe(document.results_digest);
  expect(report.claims[0].difference_mean_milli).toBe(
    document.results.comparison.metrics[0].difference.mean_milli,
  );
  await page.locator("td", { hasText: "total pnl" }).click();
  await expect(page.getByText(`experiment:${report.experiment_id}:metric:total_pnl`)).toBeVisible();
  await page
    .getByRole("textbox", { name: "Follow-up question" })
    .fill("Which maker had lower drawdown?");
  await page.getByRole("button", { name: "Ask" }).click();
  await expect(
    page.getByText("The native evidence table contains the paired result."),
  ).toBeVisible();
  const { violations } = await new AxeBuilder({ page })
    .withTags(["wcag2a", "wcag2aa", "wcag21aa"])
    .analyze();
  expect(violations.map((item) => item.id)).toEqual([]);
  await page.getByRole("link", { name: `Open experiment ${report.experiment_id}` }).click();
  await expect(page).toHaveURL(new RegExp(`/experiments/${report.experiment_id}$`));
});
