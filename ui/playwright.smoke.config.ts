import { defineConfig } from "@playwright/test";

const baseURL = process.env.AGENT_E2E_UI_BASE_URL || "http://127.0.0.1:5173";

export default defineConfig({
  testDir: "./e2e",
  timeout: 10 * 60 * 1000,
  expect: { timeout: 60 * 1000 },
  retries: 0,
  use: {
    baseURL,
    headless: true,
    trace: "on",
    screenshot: "on",
    video: "on",
  },
  reporter: [["list"]],
});
