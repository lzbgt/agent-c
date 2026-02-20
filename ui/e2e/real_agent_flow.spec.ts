import { test, expect } from "@playwright/test";

const requireReal = process.env.AGENT_E2E_REQUIRE_REAL === "1";

test.skip(!requireReal, "real agent flow requires AGENT_E2E_REQUIRE_REAL=1");

const promptSimple = `
Say hello and confirm you are ready.
`.trim();

const agentdBase = process.env.AGENT_E2E_AGENTD_BASE_URL || "http://127.0.0.1:8123";

test("real agent flow: new session run completes", async ({ page }) => {
  // Keep the test deterministic regardless of local UI defaults.
  await page.addInitScript(() => {
    try {
      window.localStorage.setItem("agentui.allowClientRpcs", "true");
      window.localStorage.setItem("agentui.allowClientEffects", "true");
      window.localStorage.setItem("agentui.allowAutoplay", "true");
      window.localStorage.setItem("agentui.tools", JSON.stringify("host"));
      window.localStorage.setItem("agentui.maxSteps", "6");
      window.localStorage.setItem("agentui.timeoutMs", "60000");
      window.localStorage.setItem("agentui.showSettings", "false");
    } catch {
      // ignore
    }
  });
  await page.goto("/");

  // Ensure the UI has a prompt box and run button.
  await expect(page.getByTestId("prompt")).toBeVisible();
  await expect(page.getByTestId("run")).toBeVisible();

  // Ensure the run is session-backed in a fresh agentd state dir.
  // The e2e harness starts agentd with an isolated state dir, so there is no pre-existing "default" session.
  await page.getByRole("button", { name: "Settings" }).click();
  await expect(page.getByTestId("settings-drawer")).toBeVisible();
  await expect(page.getByTestId("new-session")).toBeVisible();
  await page.getByTestId("new-session").click();
  await expect(page.getByTestId("scene-session")).toContainText("session=", { timeout: 30_000 });
  await expect(page.getByTestId("scene-session")).not.toContainText("session=default", { timeout: 30_000 });
  const sessionText = await page.getByTestId("scene-session").innerText({ timeout: 30_000 });
  const sessionId = (sessionText.match(/session=([A-Za-z0-9-_.]+)/) || [])[1] || "default";
  await page.getByTestId("settings-close").click();
  await page.getByTestId("settings-drawer").waitFor({ state: "detached" });

  // Run a prompt and wait for completion.
  await page.getByTestId("prompt").fill(promptSimple);
  const runButton = page.getByTestId("run");
  await runButton.click();
  try {
    await expect(runButton).toBeDisabled({ timeout: 10_000 });
  } catch {
    // Ignore if the job starts and ends too quickly to observe the disabled state.
  }
  await expect(runButton).toBeEnabled({ timeout: 4 * 60 * 1000 });

  await expect
    .poll(
      async () => {
        const resp = await page.request.get(
          `${agentdBase}/api/v1/session?session_id=${encodeURIComponent(sessionId)}`,
        );
        if (!resp.ok()) return false;
        const json = await resp.json();
        const messages = Array.isArray(json?.messages) ? json.messages : [];
        return messages.some((m) => m && m.role === "assistant" && String(m.content || "").trim().length > 0);
      },
      { timeout: 4 * 60 * 1000 },
    )
    .toBeTruthy();

  // Optional scene entity check (non-fatal if the model chooses not to render).
  try {
    await expect(page.getByTestId("scene")).toBeVisible({ timeout: 60_000 });
    const canvas = page.locator('[data-testid^="scene-entity-"] canvas').first();
    await expect(canvas).toBeVisible({ timeout: 60_000 });
    const dims = await canvas.evaluate((el) => ({ w: (el as HTMLCanvasElement).width, h: (el as HTMLCanvasElement).height }));
    expect(dims.w).toBeGreaterThan(0);
    expect(dims.h).toBeGreaterThan(0);
  } catch {
    // Optional scene validation failed; do not fail the test.
  }
});
