import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { defineConfig } from "@playwright/test";

// End-to-end tests drive the real stack: the terminal, the research API, and the C++
// engine behind it, in a workspace that starts empty. The only stand-in is the live feed:
// a local mock of the provider's stream, so no credentials or market hours are needed.
const API_PORT = 8011;
const WEB_PORT = 5183;
const MOCK_FEED_PORT = 8766;
process.env.MARKETTWIN_E2E_WORKSPACE ??= mkdtempSync(join(tmpdir(), "markettwin-e2e-"));

export default defineConfig({
  testDir: "./e2e",
  fullyParallel: false,
  workers: 1,
  timeout: 120_000,
  expect: { timeout: 15_000 },
  reporter: [["list"]],
  outputDir: "test-results",
  use: {
    baseURL: `http://127.0.0.1:${WEB_PORT}`,
    viewport: { width: 1440, height: 900 },
    trace: "retain-on-failure",
    screenshot: "only-on-failure",
  },
  webServer: [
    {
      // The local stand-in for the provider's stream. Its data is fabricated.
      command: `uv run python -m markettwin_api.live_mock --port ${MOCK_FEED_PORT}`,
      cwd: "../../services/api",
      url: `http://127.0.0.1:${MOCK_FEED_PORT}/health`,
      reuseExistingServer: false,
      timeout: 180_000,
    },
    {
      command: `uv run python -m markettwin_api --port ${API_PORT}`,
      cwd: "../../services/api",
      env: {
        MARKETTWIN_WORKSPACE: process.env.MARKETTWIN_E2E_WORKSPACE,
        MARKETTWIN_AI_PROVIDER: "fixture",
        MARKETTWIN_AI_MODEL: "deterministic-fixture",
        MARKETTWIN_LIVE_FEED: "mock",
        MARKETTWIN_LIVE_MOCK_URL: `ws://127.0.0.1:${MOCK_FEED_PORT}/v2/mock`,
      },
      url: `http://127.0.0.1:${API_PORT}/api/v1/health`,
      reuseExistingServer: false,
      timeout: 180_000,
    },
    {
      command: "node node_modules/vite/bin/vite.js",
      env: {
        MARKETTWIN_API: `http://127.0.0.1:${API_PORT}`,
        MARKETTWIN_WEB_PORT: String(WEB_PORT),
      },
      url: `http://127.0.0.1:${WEB_PORT}`,
      reuseExistingServer: false,
      timeout: 60_000,
    },
  ],
});
