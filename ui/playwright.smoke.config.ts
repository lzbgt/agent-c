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

function resolveOutputDir(): string {
  if (process.env.AGENT_E2E_OUTPUT_DIR) return process.env.AGENT_E2E_OUTPUT_DIR;
  return path.resolve(process.cwd(), "test-results", `run-${process.pid}`);
}

const baseURL = process.env.AGENT_E2E_UI_BASE_URL || readDevstackUiBase() || "http://127.0.0.1:5173";

export default defineConfig({
  testDir: "./e2e",
  outputDir: resolveOutputDir(),
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
