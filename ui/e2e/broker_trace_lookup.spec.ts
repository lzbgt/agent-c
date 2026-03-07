import { test, expect } from "@playwright/test";
import { seedBrokerState } from "./brokerTestState";

test("trace lookup panel renders in broker mode", async ({ page }) => {
  await seedBrokerState(page, { brokerPanelOpen: false, traceLookupOpen: true });

  await page.goto("/");

  await expect(page.getByText("Trace lookup")).toBeVisible();
  await expect(page.getByPlaceholder("trace_id (e.g. trace_...)")).toBeVisible();
});
