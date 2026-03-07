import { test, expect } from "@playwright/test";
import { seedBrokerState } from "./brokerTestState";

test("broker console shows members + audit panels", async ({ page }) => {
  await seedBrokerState(page);

  await page.goto("/");

  // Broker console panel should be present in broker mode.
  await expect(page.getByText("Broker console")).toBeVisible();

  // The sections should render even if network calls fail.
  await expect(page.getByText("Agents", { exact: true })).toBeVisible();
  await expect(page.getByText("Members", { exact: true })).toBeVisible();
  await expect(page.getByText("Membership audit", { exact: true })).toBeVisible();
});
