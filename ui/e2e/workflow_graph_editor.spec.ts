import { test, expect } from "@playwright/test";

test("workflow graph editor renders and can add nodes", async ({ page }) => {
  await page.addInitScript(() => {
    try {
      window.localStorage.setItem("agentui.simpleMode", "false");
      window.localStorage.setItem("agentui.workflowPanelOpen", JSON.stringify(true));
      window.localStorage.setItem("agentui.workflowComposerMode", JSON.stringify("graph"));
      window.localStorage.setItem("agentui.connectionMode", JSON.stringify("broker"));
    } catch {
      // ignore
    }
  });

  await page.goto("/");

  await expect(page.getByText("Workflows", { exact: true })).toBeVisible();
  await page.getByTestId("workflow-composer-tab-graph").click();

  const addLlm = page.getByTestId("workflow-graph-add-llm");
  await expect(addLlm).toBeVisible();

  await addLlm.click();
  await expect(page.getByTestId("workflow-graph-inspector")).toBeVisible();
  const firstId = (await page.getByTestId("workflow-graph-node-id").inputValue()).trim();

  await page.getByTestId("workflow-graph-export-json").click();
  const json = await page.getByTestId("workflow-composer-json").inputValue();
  expect(json).toContain(`\"task_id\": \"${firstId}\"`);
});
