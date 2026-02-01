import { test, expect } from "@playwright/test";
import fs from "node:fs";
import path from "node:path";

function nowId() {
  const d = new Date();
  const pad = (n: number) => String(n).padStart(2, "0");
  return `${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}_${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}`;
}

test("observe: canvas2d function-expression scripts execute (open-world)", async ({ page }) => {
  const outDir = process.env.AGENT_E2E_OUT_DIR || path.join(process.cwd(), "..", "out", `observe_canvas_${nowId()}`);
  fs.mkdirSync(outDir, { recursive: true });

  const agentdBase = process.env.AGENT_E2E_AGENTD_BASE_URL || "http://127.0.0.1:8123";
  const providerBaseUrl = process.env.AGENT_E2E_PROVIDER_BASE_URL || "";
  const providerModel = process.env.AGENT_E2E_PROVIDER_MODEL || "";
  const providerApiKey = process.env.AGENT_E2E_PROVIDER_API_KEY || "";

  const pageErrors: string[] = [];
  const file404s: string[] = [];

  page.on("pageerror", (e) => {
    pageErrors.push(String(e));
  });
  page.on("response", async (r) => {
    try {
      const url = r.url();
      if (!url.includes("/api/v1/file?")) return;
      if (r.status() === 404) file404s.push(url);
    } catch {
      // ignore
    }
  });

  await page.addInitScript((cfg) => {
    try {
      window.localStorage.setItem("agentui.base", JSON.stringify(cfg.agentdBase));
      if (cfg.providerBaseUrl) window.localStorage.setItem("agentui.baseUrl", JSON.stringify(cfg.providerBaseUrl));
      if (cfg.providerModel) window.localStorage.setItem("agentui.model", JSON.stringify(cfg.providerModel));
      if (cfg.providerApiKey) window.localStorage.setItem("agentui.apiKey", JSON.stringify(cfg.providerApiKey));
      window.localStorage.setItem("agentui.allowClientRpcs", "true");
      window.localStorage.setItem("agentui.allowClientEffects", "true");
      window.localStorage.setItem("agentui.allowUnsafePageEval", "true");
      window.localStorage.setItem("agentui.allowAutoplay", "true");
      window.localStorage.setItem("agentui.verbose", "true");
      window.localStorage.setItem("agentui.showDebugInConversation", "true");
      window.localStorage.setItem("agentui.trace", "true");
      window.localStorage.setItem("agentui.tools", JSON.stringify("host"));
      window.localStorage.setItem("agentui.showSettings", "false");
      window.localStorage.setItem("agentui.yolo", "true");
      window.localStorage.setItem("agentui.hostPolicy", JSON.stringify("full"));
    } catch {
      // ignore
    }
  }, { agentdBase, providerBaseUrl, providerModel, providerApiKey });

  await page.goto("/", { waitUntil: "domcontentloaded" });
  await page.screenshot({ path: path.join(outDir, "page_0s.png"), fullPage: true });

  const sessionText = await page.getByTestId("scene-session").innerText({ timeout: 60_000 });
  const sessionId = (sessionText.match(/session=([A-Za-z0-9-_.]+)/) || [])[1] || "default";

  await page.getByTestId("prompt").fill("nice, present a sine fun with time advances plot for me", { timeout: 30_000 });
  await page.getByTestId("run").click({ timeout: 30_000 });

  // Wait for the canvas entity to appear.
  const entity = page.getByTestId("scene-entity-sine-wave");
  await expect(entity).toBeVisible({ timeout: 120_000 });

  // Validate the canvas script actually executed by sampling a pixel:
  // - When the script runs, it paints a non-transparent background.
  // - Without execution (the regression), the canvas stays transparent after clearRect.
  const ok = await page.waitForFunction(
    () => {
      const root = document.querySelector('[data-testid="scene-entity-sine-wave"]') as HTMLElement | null;
      if (!root) return false;
      const canvas = root.querySelector("canvas") as HTMLCanvasElement | null;
      if (!canvas) return false;
      const ctx = canvas.getContext("2d");
      if (!ctx) return false;
      const p = ctx.getImageData(0, 0, 1, 1).data;
      const alpha = Number(p?.[3] ?? 0);
      return alpha > 0;
    },
    undefined,
    { timeout: 120_000 },
  );
  expect(ok).toBeTruthy();

  await page.screenshot({ path: path.join(outDir, "page.png"), fullPage: true });

  expect(pageErrors, `page errors: ${pageErrors.join("\n")}`).toEqual([]);
  expect(file404s, `404s for /api/v1/file: ${file404s.join("\n")}`).toEqual([]);

  // Scene scripts should not error.
  const ev = await page.request.get(
    `${agentdBase}/api/v1/session/client_events?session_id=${encodeURIComponent(sessionId)}&max_bytes=1048576`,
  );
  const evJson = await ev.json();
  const items: any[] = Array.isArray(evJson?.events)
    ? evJson.events
    : Array.isArray(evJson?.items)
      ? evJson.items
      : [];
  const sceneErrors = items.filter((it) => it && it.type === "scene_error");
  expect(sceneErrors.length, "scene_error events observed").toBe(0);
});

