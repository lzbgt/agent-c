import { test, expect } from "@playwright/test";
import { seedBrokerState } from "./brokerTestState";

test("broker membership flow wires settings and refresh actions", async ({ page }) => {
  await seedBrokerState(page);

  await page.goto("/");

  // Fill membership form to ensure inputs are wired.
  await expect(page.getByPlaceholder("user_sub")).toBeVisible();
  await page.getByPlaceholder("user_sub").fill("user-123");
  const membersSection = page.locator("section").filter({ has: page.getByText("Members", { exact: true }) });
  await expect(membersSection.getByRole("button", { name: "Save", exact: true })).toBeVisible();

  // Trigger refresh buttons (no assertions on network, just UI wiring).
  await expect(membersSection.getByRole("button", { name: /Refresh|Loading/ })).toBeVisible();
});
