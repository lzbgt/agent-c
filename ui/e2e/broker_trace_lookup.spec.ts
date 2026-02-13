import { test, expect } from "@playwright/test";

test("trace lookup panel renders in broker mode", async ({ page }) => {
  await page.addInitScript(() => {
    try {
      window.localStorage.setItem("agentui.connectionMode", JSON.stringify("broker"));
      window.localStorage.setItem("agentui.brokerBase", "https://broker.example.invalid");
      window.localStorage.setItem("agentui.brokerAuthToken", "test-token");
      window.localStorage.setItem("agentui.brokerAgentId", "agent1");
      window.localStorage.setItem("agentui.traceLookupOpen", "true");
      window.localStorage.setItem("agentui.allowClientRpcs", "true");
      window.localStorage.setItem("agentui.allowClientEffects", "true");
    } catch {
      // ignore
    }
  });

  await page.goto("/");

  await expect(page.getByText("Trace lookup")).toBeVisible();
  await expect(page.getByPlaceholder("trace_id (e.g. trace_...)")).toBeVisible();
});
