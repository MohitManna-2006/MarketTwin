import AxeBuilder from "@axe-core/playwright";
import { expect, test } from "@playwright/test";

test("imports and replays the offline historical fixture", async ({ page }) => {
  await page.goto("/historical");
  await expect(page.getByRole("heading", { name: /Historical session library/ })).toBeVisible();
  await expect(page.getByText(/Imports never enter the synthetic exchange/)).toBeVisible();

  await page.getByRole("button", { name: "Import observations" }).click();
  // Other specs share the workspace and may have imported windowed AAPL sessions.
  const row = page.getByRole("row").filter({ hasText: "bundled offline fixture" });
  await expect(row.getByText("Completed")).toBeVisible();
  await row.click();

  await expect(page.getByText("FABRICATED TEST DATA")).toBeVisible();
  await expect(page.getByText(/Quotes are recorded observations/)).toBeVisible();
  await page.getByRole("button", { name: "Verify replay" }).click();
  await expect(page.getByText("Native deterministic replay verified.")).toBeVisible();
  const accessibility = await new AxeBuilder({ page })
    .withTags(["wcag2a", "wcag2aa", "wcag21a", "wcag21aa"])
    .analyze();
  expect(accessibility.violations).toEqual([]);
});
