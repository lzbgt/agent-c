import { test, expect } from "@playwright/test";

test("broker membership flow wires settings and refresh actions", async ({ page }) => {
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

  // Fill membership form to ensure inputs are wired.
  await expect(page.getByPlaceholder("user_sub")).toBeVisible();
  await page.getByPlaceholder("user_sub").fill("user-123");
  await expect(page.getByRole("button", { name: "Save" })).toBeVisible();

  // Trigger refresh buttons (no assertions on network, just UI wiring).
  const membersSection = page.locator("section").filter({ has: page.getByText("Members", { exact: true }) });
  await expect(membersSection.getByRole("button", { name: /Refresh|Loading/ })).toBeVisible();
});
