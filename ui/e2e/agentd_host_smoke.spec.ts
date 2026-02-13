import { test, expect } from "@playwright/test";

test("agentd host smoke renders core UI", async ({ page }) => {
  await page.addInitScript(() => {
    try {
      window.localStorage.setItem("agentui.connectionMode", JSON.stringify("direct"));
      window.localStorage.setItem("agentui.showSettings", "false");
      window.localStorage.setItem("agentui.allowClientRpcs", "true");
      window.localStorage.setItem("agentui.allowClientEffects", "true");
    } catch {
      // ignore
    }
  });

  await page.goto("/");

  await expect(page.getByTestId("prompt")).toBeVisible();
  await expect(page.getByTestId("run")).toBeVisible();
});
