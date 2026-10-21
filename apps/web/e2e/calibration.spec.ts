import AxeBuilder from "@axe-core/playwright";
import { expect, test } from "@playwright/test";

test("calibrates against fabricated sessions and labels the result as fabricated", async ({
  page,
}) => {
  await page.goto("/calibration");
  await expect(page.getByRole("heading", { name: /New calibration/ })).toBeVisible();
  await page.getByRole("button", { name: "Import fabricated sessions" }).click();
  await expect(page.getByText("10 of 10 sessions selected", { exact: false })).toBeVisible({
    timeout: 60_000,
  });
  await page.getByRole("button", { name: "Run calibration" }).click();

  const outcome = page.getByRole("region", { name: "Outcome" });
  await expect(outcome).toBeVisible({ timeout: 90_000 });
  await expect(outcome.getByText(/FABRICATED TEST/).first()).toBeVisible();
  await expect(
    page.getByRole("heading", { name: /Stylized facts on the test sessions/ }),
  ).toBeVisible();
  await page.getByRole("button", { name: "Verify artifact" }).click();
  await expect(page.getByRole("button", { name: "Verified" })).toBeVisible({ timeout: 60_000 });

  const accessibility = await new AxeBuilder({ page })
    .withTags(["wcag2a", "wcag2aa", "wcag21a", "wcag21aa"])
    .analyze();
  expect(accessibility.violations).toEqual([]);
});
