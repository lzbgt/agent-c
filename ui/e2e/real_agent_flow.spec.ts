import { test, expect } from "@playwright/test";

const promptSine = `
Draw a sine plot in the Web UI Scene.

Use a canvas2d entity with:
- id: plot-1
- width: 640
- height: 240
- title: Sine plot

Use ` + "`props.script`" + ` (JavaScript) to draw the sine wave on the canvas using (ctx,width,height).
`.trim();

const promptPpt = `
Create a PowerPoint .pptx (one slide) that says "Hello World".

Requirements:
- Use host tools to create a real file at path "out/hello_world.pptx" under tools root.
- If dependencies are missing, install them autonomously into ./.agent_deps/py (python venv) and use that.
- Register the file as an artifact so the UI shows a download link.
- When the link is visible, treat that as DoD and STOP (do not loop).
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

  // Expect the Scene to show "plot-1" eventually.
  await expect(page.getByTestId("scene")).toBeVisible();
  await expect(page.getByTestId("scene-entity-plot-1")).toBeVisible({ timeout: 4 * 60 * 1000 });

  // PPTX artifact.
  await page.getByTestId("prompt").fill(promptPpt);
  await page.getByTestId("run").click();

  // Expect a downloadable link for the pptx to appear.
  await expect(page.locator('a[download="hello_world.pptx"]')).toBeVisible({ timeout: 6 * 60 * 1000 });
});
