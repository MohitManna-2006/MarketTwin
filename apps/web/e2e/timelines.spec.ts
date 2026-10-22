import AxeBuilder from "@axe-core/playwright";
import { expect, test } from "@playwright/test";

test("an AI desk run forks into a verified timeline", async ({ page }) => {
  test.setTimeout(180_000);
  await page.goto("/desks/new");
  await expect(page.getByRole("heading", { name: /Hand one agent to a model/ })).toBeVisible();
  await page.getByRole("button", { name: "Start the desk" }).click();

  await expect(page.getByText("FIXTURE MODEL")).toBeVisible({ timeout: 60_000 });
  await expect(page.getByRole("heading", { name: /Decisions/ }).first()).toBeVisible();
  const accessibility = await new AxeBuilder({ page })
    .withTags(["wcag2a", "wcag2aa", "wcag21a", "wcag21aa"])
    .analyze();
  expect(accessibility.violations).toEqual([]);

  await page.getByRole("link", { name: "Timelines" }).click();
  await expect(page.getByRole("heading", { name: /Fork this run/ })).toBeVisible();
  await page.getByRole("button", { name: "Fork this run" }).click();
  await expect(page.getByText(/Prefix verified/)).toBeVisible({ timeout: 90_000 });
  await expect(page.getByText("ONE SAMPLE PATH").first()).toBeVisible();
  await expect(page.getByText("First market divergence")).toBeVisible();
  const forkAccessibility = await new AxeBuilder({ page })
    .withTags(["wcag2a", "wcag2aa", "wcag21a", "wcag21aa"])
    .analyze();
  expect(forkAccessibility.violations).toEqual([]);
});
