import { test, expect } from "@playwright/test";

test("workflow graph editor renders and can add nodes", async ({ page }) => {
  await page.addInitScript(() => {
    try {
      window.localStorage.setItem("agentui.workflowPanelOpen", JSON.stringify(true));
      window.localStorage.setItem("agentui.workflowComposerMode", JSON.stringify("graph"));
      window.localStorage.setItem("agentui.connectionMode", JSON.stringify("broker"));
    } catch {
      // ignore
    }
  });

  await page.goto("/");

  await expect(page.getByText("Workflows", { exact: true })).toBeVisible();
  const graphTab = page.getByRole("button", { name: "Graph" });
  await graphTab.click();

  const addLlm = page.getByRole("button", { name: "Add LLM node" });
  await expect(addLlm).toBeVisible();

  await addLlm.click();
  await expect(page.getByText("Node inspector")).toBeVisible();
});
