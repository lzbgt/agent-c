import { test, expect } from "@playwright/test";

test("broker console shows members + audit panels", async ({ page }) => {
  await page.addInitScript(() => {
    try {
      window.localStorage.setItem("agentui.connectionMode", JSON.stringify("broker"));
      window.localStorage.setItem("agentui.brokerBase", "https://broker.example.invalid");
      window.localStorage.setItem("agentui.brokerAuthToken", "test-token");
      window.localStorage.setItem("agentui.brokerAgentId", "agent1");
      window.localStorage.setItem("agentui.brokerPanelOpen", "true");
      window.localStorage.setItem("agentui.showSettings", "false");
      window.localStorage.setItem("agentui.allowClientRpcs", "true");
      window.localStorage.setItem("agentui.allowClientEffects", "true");
    } catch {
      // ignore
    }
  });

  await page.goto("/");

  // Broker console panel should be present in broker mode.
  await expect(page.getByText("Broker console")).toBeVisible();

  // The sections should render even if network calls fail.
  await expect(page.getByText("Agents", { exact: true })).toBeVisible();
  await expect(page.getByText("Members", { exact: true })).toBeVisible();
  await expect(page.getByText("Membership audit", { exact: true })).toBeVisible();
});
