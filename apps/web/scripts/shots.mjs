// Captures screenshots of the running terminal for design review.
//
//   node scripts/shots.mjs <out-dir> <name>=<path>[@<width>x<height>] ...
//
// Each target may be followed by actions in the URL fragment-free form
// "name=/path@1440x900". The dev server must be running on 127.0.0.1:5173.

import { mkdirSync } from "node:fs";
import { chromium } from "@playwright/test";

const [outDir, ...targets] = process.argv.slice(2);
if (!outDir || targets.length === 0) {
  console.error("usage: node scripts/shots.mjs <out-dir> <name>=<path>[@WxH] ...");
  process.exit(2);
}
mkdirSync(outDir, { recursive: true });
const base = process.env.TERMINAL_URL ?? "http://127.0.0.1:5173";
const browser = await chromium.launch();
for (const target of targets) {
  const name = target.slice(0, target.indexOf("="));
  const rest = target.slice(target.indexOf("=") + 1);
  const [path, size = "1440x900"] = rest.split("@");
  const [width, height] = size.split("x").map(Number);
  const context = await browser.newContext({ viewport: { width, height }, deviceScaleFactor: 1 });
  const page = await context.newPage();
  const errors = [];
  page.on("console", (message) => {
    if (message.type() === "error") errors.push(message.text());
  });
  page.on("pageerror", (error) => errors.push(String(error)));
  await page.goto(base + path, { waitUntil: "networkidle" });
  await page.waitForTimeout(Number(process.env.SHOT_WAIT ?? 600));
  const file = `${outDir}/${name}.png`;
  await page.screenshot({ path: file });
  console.log(
    `${file}  ${width}x${height}${errors.length ? `  console errors: ${errors.slice(0, 3).join(" | ")}` : ""}`,
  );
  await context.close();
}
await browser.close();
