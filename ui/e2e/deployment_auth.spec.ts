import { expect, test } from "@playwright/test";

test("deployed sign-in gates execution and has a responsive provider surface", async ({ page }) => {
  await page.addInitScript(() => { window.__AGENT_UI_CONFIG__ = { identityEnabled: true }; });
  let runtimeRequests = 0;
  await page.route("**/api/v1/**", async (route) => { runtimeRequests++; await route.fulfill({ json: { ok: true } }); });
  await page.route("**/auth/session", (route) => route.fulfill({ json: { authenticated: false } }));
  await page.route("**/auth/providers", (route) => route.fulfill({ json: { apple: true, github: true, google: true } }));
  await page.goto("/");
  await expect(page.getByRole("heading", { name: "From intent to verified work." })).toBeVisible();
  await expect(page.getByRole("link", { name: "Continue with Apple" })).toHaveAttribute("href", "/auth/apple/start");
  await expect(page.getByRole("link", { name: "Continue with Google" })).toHaveAttribute("href", "/auth/google/start");
  await expect(page.getByRole("navigation", { name: "Workspace navigation" })).toHaveCount(0);
  expect(runtimeRequests).toBe(0);
  await page.screenshot({ path: "test-results/deployment-sign-in-desktop.png", fullPage: true });
  await page.setViewportSize({ width: 390, height: 844 });
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await page.screenshot({ path: "test-results/deployment-sign-in-mobile.png", fullPage: true });
});

test("signed-in members cannot mount the execution workspace", async ({ page }) => {
  await page.addInitScript(() => { window.__AGENT_UI_CONFIG__ = { identityEnabled: true }; });
  await page.route("**/auth/session", (route) => route.fulfill({ json: { authenticated: true, admin: false } }));
  await page.goto("/");
  await expect(page.getByRole("heading", { name: "Your account is signed in" })).toBeVisible();
  await expect(page.getByRole("navigation", { name: "Workspace navigation" })).toHaveCount(0);
  await expect(page.getByRole("button", { name: "Sign in with another account" })).toBeVisible();
});

test("verified administrators reach the workspace with an explicit sign-out control", async ({ page }) => {
  await page.addInitScript(() => {
    window.__AGENT_UI_CONFIG__ = { identityEnabled: true };
    localStorage.setItem("agentui.base", "http://127.0.0.1:7777");
    localStorage.setItem("agentui.showSettings", "false");
  });
  await page.route("**/auth/session", (route) => route.fulfill({ json: { authenticated: true, admin: true, display_name: "System administrator" } }));
  await page.route("**/api/v1/**", (route) => route.fulfill({ json: { ok: true, service: "agentd", sessions: [], tools: [], messages: [], runs: [], events: [], entities: [], skills: [], workflows: [], schedules: [], approvals: [], prefs: {} } }));
  await page.goto("/");
  await expect(page.getByRole("navigation", { name: "Workspace navigation" })).toBeVisible();
  await expect(page.getByRole("button", { name: "Sign out of workspace" })).toBeVisible();
  for (const width of [320, 390, 768]) {
    await page.setViewportSize({ width, height: 844 });
    await expect(page.getByRole("button", { name: "Sign out of workspace" })).toBeInViewport({ ratio: 1 });
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  }
});
