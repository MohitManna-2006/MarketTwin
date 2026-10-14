import AxeBuilder from "@axe-core/playwright";
import { expect, test } from "@playwright/test";

// The API is pointed at the local mock Alpaca stream (see playwright.config.ts). Every
// figure here is fabricated, and the terminal must say so.
test("streams the mock feed, records it, and files the recording as a historical session", async ({
  page,
  request,
}) => {
  await page.goto("/live");
  await page.getByRole("radiogroup", { name: "Feed" }).getByRole("radio", { name: "Mock" }).click();
  await page.getByRole("button", { name: "Connect feed" }).click();

  await expect(page.getByTestId("source-label")).toHaveText("FABRICATED MOCK DATA");
  await expect(page.getByText("Feed connected").first()).toBeVisible();
  await expect(page.getByRole("button", { name: "Inspect FAKEPACA" })).toBeVisible();

  const accessibility = await new AxeBuilder({ page })
    .withTags(["wcag2a", "wcag2aa", "wcag21a", "wcag21aa"])
    .analyze();
  expect(accessibility.violations).toEqual([]);

  await page.getByRole("button", { name: "Record" }).click();
  await expect(page.getByRole("button", { name: "Stop recording" })).toBeVisible();
  await page.waitForTimeout(2_000);
  await page.getByRole("button", { name: "Stop recording" }).click();
  await expect(page.getByRole("button", { name: "Record" })).toBeVisible();

  await expect
    .poll(
      async () => {
        const response = await request.get("/api/v1/live/recordings");
        const body = (await response.json()) as { recordings: { status: string }[] };
        return body.recordings[0]?.status;
      },
      { timeout: 30_000 },
    )
    .toBe("completed");

  await page.getByRole("button", { name: "Disconnect feed" }).click();
  await expect(page.getByText("Feed stopped").first()).toBeVisible();

  await page.goto("/historical");
  const row = page.getByRole("row").filter({ hasText: "FAKEPACA" });
  await expect(row.getByText("Completed")).toBeVisible();
  await row.click();
  await expect(page.getByText("FABRICATED TEST DATA")).toBeVisible();
});
