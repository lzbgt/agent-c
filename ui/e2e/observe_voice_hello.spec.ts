import { test } from "@playwright/test";
import fs from "node:fs";
import path from "node:path";

const requireVoice = process.env.AGENT_E2E_REQUIRE_VOICE === "1";
test.skip(!requireVoice, "voice e2e requires AGENT_E2E_REQUIRE_VOICE=1 with a voice-capable provider");

function nowId() {
  const d = new Date();
  const pad = (n: number) => String(n).padStart(2, "0");
  return `${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}_${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}`;
}

test("observe: say hello in voice (open-world)", async ({ page }) => {
  const outDir = process.env.AGENT_E2E_OUT_DIR || path.join(process.cwd(), "..", "out", `observe_${nowId()}`);
  fs.mkdirSync(outDir, { recursive: true });

  const agentdBase = process.env.AGENT_E2E_AGENTD_BASE_URL || "http://127.0.0.1:8123";
  const providerBaseUrl = process.env.AGENT_E2E_PROVIDER_BASE_URL || "";
  const providerModel = process.env.AGENT_E2E_PROVIDER_MODEL || "";
  const providerApiKey = process.env.AGENT_E2E_PROVIDER_API_KEY || "";

  const consoleLines: string[] = [];
  const pageErrors: string[] = [];
  const requestFailures: string[] = [];

  const progressPath = path.join(outDir, "progress.log");
  const progress = (msg: string) => {
    const line = `[${new Date().toISOString()}] ${msg}\n`;
    try {
      fs.appendFileSync(progressPath, line, "utf8");
    } catch {
      // ignore
    }
  };

  page.on("console", (m) => {
    const line = `[console:${m.type()}] ${m.text()}`;
    consoleLines.push(line);
  });
  page.on("pageerror", (e) => {
    pageErrors.push(String(e));
  });
  page.on("requestfailed", (r) => {
    requestFailures.push(`${r.method()} ${r.url()} -> ${r.failure()?.errorText || "failed"}`);
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

  let sessionId = "default";

  try {
    progress(`navigating to / (baseURL=${process.env.AGENT_E2E_UI_BASE_URL || "unset"})`);
    await page.goto("/", { waitUntil: "domcontentloaded" });
    progress("loaded domcontent");

    // Always capture at least one early screenshot, even if later steps hang/fail.
    await page.screenshot({ path: path.join(outDir, "page_0s.png"), fullPage: true });

    progress("waiting for scene session label");
    const sessionText = await page.getByTestId("scene-session").innerText({ timeout: 60_000 });
    sessionId = (sessionText.match(/session=([A-Za-z0-9-_.]+)/) || [])[1] || "default";
    progress(`sessionId=${sessionId}`);

    progress("submitting prompt");
    await page.getByTestId("prompt").fill("say hello in voice to me", { timeout: 30_000 });
    await page.getByTestId("run").click({ timeout: 30_000 });

    // Open-world observation: wait and periodically snapshot the UI.
    const started = Date.now();
    const deadlineMs = 6 * 60 * 1000;
    let lastSnap = 0;
    while (Date.now() - started < deadlineMs) {
      const now = Date.now();
      if (now - lastSnap > 10_000) {
        lastSnap = now;
        await page.screenshot({ path: path.join(outDir, `page_${Math.floor((now - started) / 1000)}s.png`), fullPage: true });
        progress(`snapshot t=${Math.floor((now - started) / 1000)}s`);
      }

      // Stop early only after the run is done and we see any artifact UI or media element,
      // and there are no obvious scene errors.
      const runBtnText = (await page.getByTestId("run").innerText().catch(() => "")).trim().toLowerCase();
      const cancelVisible = await page
        .getByRole("button", { name: "Cancel" })
        .isVisible()
        .catch(() => false);
      const isRunning = cancelVisible || runBtnText.includes("running");

      const haveArtifact = (await page.locator('text="Artifact:"').count()) > 0;
      const haveDownload = (await page.locator('a[download]').count()) > 0;
      const haveAudio = (await page.locator("audio").count()) > 0;
      const haveSceneError = (await page.locator('text="canvas script error"').count()) > 0;
      // Prefer the durable Scene-based player: it should manifest as a real <audio> element in the page.
      if (!isRunning && haveAudio && !haveSceneError) {
        progress("early-stop condition met; waiting 5s for settle");
        await page.waitForTimeout(5000);
        break;
      }

      await page.waitForTimeout(1000);
    }

    // Minimal end-state assertions (still "open-world", but we must ensure something was actually presented).
    const finalHaveAudio = (await page.locator("audio").count()) > 0;
    if (!finalHaveAudio) {
      throw new Error("expected at least one <audio> element rendered in the WebUI");
    }

    // Scene scripts should not error for the core “say hello in voice” flow.
    try {
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
      if (sceneErrors.length > 0) {
        throw new Error(`scene_error events observed (${sceneErrors.length})`);
      }
    } catch (e) {
      // If the endpoint is unavailable, keep the artifacts and fail so we notice.
      throw new Error(`failed client_events/scene_error check: ${String(e)}`);
    }
  } catch (e) {
    progress(`ERROR: ${String(e)}`);
    throw e;
  } finally {
    // Final screenshot + all logs are best-effort.
    try {
      await page.screenshot({ path: path.join(outDir, "page.png"), fullPage: true });
    } catch {
      // ignore
    }

    try {
      fs.writeFileSync(path.join(outDir, "console.log"), consoleLines.join("\n") + "\n", "utf8");
      fs.writeFileSync(path.join(outDir, "page_errors.log"), pageErrors.join("\n") + "\n", "utf8");
      fs.writeFileSync(path.join(outDir, "request_failures.log"), requestFailures.join("\n") + "\n", "utf8");
    } catch {
      // ignore
    }

    // Pull server-side session audit + client events for postmortem (best-effort).
    try {
      const audit = await page.request.get(
        `${agentdBase}/api/v1/session/audit?session_id=${encodeURIComponent(sessionId)}&max_bytes=1048576`,
      );
      const auditText = await audit.text();
      fs.writeFileSync(path.join(outDir, "agentd_audit.json"), auditText, "utf8");
    } catch (e) {
      fs.writeFileSync(path.join(outDir, "agentd_audit.error.txt"), String(e), "utf8");
    }

    try {
      const ev = await page.request.get(
        `${agentdBase}/api/v1/session/client_events?session_id=${encodeURIComponent(sessionId)}&max_bytes=1048576`,
      );
      const evText = await ev.text();
      fs.writeFileSync(path.join(outDir, "agentd_client_events.json"), evText, "utf8");
    } catch (e) {
      fs.writeFileSync(path.join(outDir, "agentd_client_events.error.txt"), String(e), "utf8");
    }

    // New durable Scene snapshot endpoint (post-refresh source of truth).
    try {
      const scene = await page.request.get(`${agentdBase}/api/v1/session/scene?session_id=${encodeURIComponent(sessionId)}`);
      const sceneText = await scene.text();
      fs.writeFileSync(path.join(outDir, "agentd_scene.json"), sceneText, "utf8");
    } catch (e) {
      fs.writeFileSync(path.join(outDir, "agentd_scene.error.txt"), String(e), "utf8");
    }
  }
});
