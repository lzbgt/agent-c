import { test, expect } from "@playwright/test";

const promptSine = `
Draw a sine plot and present it to me in the scene.
`.trim();

const promptPpt = `
Generate a PowerPoint (.pptx) with "hello" in the page center, and present it to me.
`.trim();

test("real agent flow: sine plot entity + pptx artifact", async ({ page }) => {
  // Keep the test deterministic regardless of local UI defaults.
  await page.addInitScript(() => {
    try {
      window.localStorage.setItem("agentui.allowClientRpcs", "true");
      window.localStorage.setItem("agentui.allowClientEffects", "true");
      window.localStorage.setItem("agentui.allowAutoplay", "true");
      window.localStorage.setItem("agentui.tools", JSON.stringify("host"));
      window.localStorage.setItem("agentui.showSettings", "true");
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
  await expect(page.getByTestId("new-session")).toBeVisible();
  await page.getByTestId("new-session").click();
  await expect(page.getByTestId("scene-session")).toContainText("session=", { timeout: 30_000 });
  await expect(page.getByTestId("scene-session")).not.toContainText("session=default", { timeout: 30_000 });

  // Sine plot.
  await page.getByTestId("prompt").fill(promptSine);
  await page.getByTestId("run").click();

  // Expect the Scene to show at least one canvas entity eventually.
  await expect(page.getByTestId("scene")).toBeVisible();
  const canvas = page.locator('[data-testid^="scene-entity-"] canvas').first();
  await expect(canvas).toBeVisible({ timeout: 4 * 60 * 1000 });
  await expect
    .poll(async () => {
      return canvas.evaluate((el) => ({ w: (el as HTMLCanvasElement).width, h: (el as HTMLCanvasElement).height }));
    })
    .toEqual({ w: 640, h: 240 });

  // PPTX artifact.
  await page.getByTestId("prompt").fill(promptPpt);
  await page.getByTestId("run").click();

  // Expect a downloadable link for the pptx to appear.
  await expect(page.locator('a[download$=".pptx"]')).toBeVisible({ timeout: 6 * 60 * 1000 });
});
