import { test, expect } from "@playwright/test";

const promptSine = `
Draw a sine plot on the client Scene.

Requirements:
- Use the client collaboration RPC: ui_action(type="client_rpc", rpc.kind="entity_apply").
- Create a canvas2d entity with id "plot-1", width 640, height 240, title "Sine plot".
- Call the canvas2d action "plot_sine" with amplitude=1, frequency=2, phase=0, samples=256.
- Wait once for client_rpc_result correlated by rpc_id (use client_wait_event) and then STOP.
`.trim();

const promptPpt = `
Create a PowerPoint .pptx (one slide) that says "Hello World".

Requirements:
- Use host tools to create a real file at path "out/hello_world.pptx" under tools root.
- If dependencies are missing, install them autonomously into ./.agent_deps/py (python venv) and use that.
- Register the file via artifact_register with kind="file" and mime="application/vnd.openxmlformats-officedocument.presentationml.presentation".
- Use the collaboration surface to show the file link in the UI if helpful (dom_apply or entity_apply).
- Wait once for artifact_rendered (or client_rpc_result) and then STOP.
`.trim();

test("real agent flow: sine plot entity + pptx artifact", async ({ page }) => {
  await page.goto("/");

  // Ensure the UI has a prompt box and run button.
  await expect(page.getByTestId("prompt")).toBeVisible();
  await expect(page.getByTestId("run")).toBeVisible();

  // Sine plot.
  await page.getByTestId("prompt").fill(promptSine);
  await page.getByTestId("run").click();

  // Expect the Scene to show "plot-1" eventually.
  await expect(page.getByText("Scene")).toBeVisible();
  await expect(page.getByText("plot-1", { exact: false })).toBeVisible({ timeout: 4 * 60 * 1000 });

  // PPTX artifact.
  await page.getByTestId("prompt").fill(promptPpt);
  await page.getByTestId("run").click();

  // Expect a downloadable link for the pptx to appear.
  await expect(page.getByText("hello_world.pptx", { exact: false })).toBeVisible({ timeout: 6 * 60 * 1000 });
});

