import fs from "node:fs";
import path from "node:path";
import { defineConfig } from "@playwright/test";

function readDevstackUiBase(): string {
  if (process.env.AGENT_E2E_PREFER_DEVSTACK === "0") return "";
  try {
    const statePath = path.resolve(process.cwd(), "..", "out", "devstack_state.json");
    const raw = fs.readFileSync(statePath, "utf8");
    const parsed = JSON.parse(raw) as { webui_base?: unknown };
    return typeof parsed.webui_base === "string" ? parsed.webui_base.trim() : "";
  } catch {
    return "";
  }
}

const defaultPort = process.env.AGENT_E2E_UI_PORT || "4173";
const devstackBaseURL = readDevstackUiBase();
const baseURL = process.env.AGENT_E2E_UI_BASE_URL || devstackBaseURL || `http://127.0.0.1:${defaultPort}`;
const useExistingServer = Boolean(process.env.AGENT_E2E_UI_BASE_URL || devstackBaseURL);

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
  webServer: useExistingServer
    ? undefined
    : {
        command: `npm run dev -- --host 127.0.0.1 --port ${defaultPort} --strictPort`,
        url: baseURL,
        reuseExistingServer: false,
        timeout: 120 * 1000,
      },
  reporter: [["list"]],
});
