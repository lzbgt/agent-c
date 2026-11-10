import { expect, test } from "@playwright/test";

test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => {
    localStorage.setItem("agentui.connectionMode", JSON.stringify("direct"));
    localStorage.setItem("agentui.base", "http://127.0.0.1:7777");
    localStorage.setItem("agentui.showSettings", "false");
  });
  await page.route("**/api/v1/**", async (route) => {
    const path = new URL(route.request().url()).pathname;
    if (path.endsWith("/workflow/submit")) {
      const payload = route.request().postDataJSON();
      expect(payload.sop.schema).toBe("agentd.sop.v1");
      expect(payload.sop.stages).toHaveLength(3);
      expect(payload.tasks).toBeUndefined();
      return route.fulfill({ json: { ok: true, workflow_id: "wf-sop-browser" } });
    }
    await route.fulfill({ json: { ok: true, service: "agentd", version: "test", features: {}, sessions: [], tools: [], messages: [], runs: [], events: [], entities: [], skills: [], workflows: [], schedules: [], approvals: [], prefs: {} } });
  });
});

test("workspace navigation retains accessible names, active state and return path", async ({ page }) => {
  await page.goto("/");
  const nav = page.getByRole("navigation", { name: "Workspace navigation" });
  await expect(nav.getByRole("button", { name: "Overview", exact: true })).toHaveAttribute("aria-current", "page");
  await expect(page.getByRole("heading", { name: "Workspace", exact: true })).toBeVisible();
  await expect(page.getByTestId("workspace-empty")).toBeVisible();
  await page.screenshot({ path: "test-results/workspace-desktop.png", fullPage: true });
  await nav.getByRole("button", { name: "Workflows", exact: true }).click();
  await expect(page.getByTestId("workspace-heading")).toContainText("Compose repeatable processes");
  await expect(nav.getByRole("button", { name: "Workflows", exact: true })).toHaveAttribute("aria-current", "page");
  await page.getByRole("button", { name: "Collapse tools", exact: true }).click();
  await nav.getByRole("button", { name: "Overview", exact: true }).click();
  await expect(page.getByTestId("workspace-empty")).toBeVisible();
  await page.getByRole("button", { name: "Start a conversation" }).click();
  await expect(page.getByTestId("prompt")).toBeFocused();
});

test("SOP authoring template reaches the existing submit API", async ({ page }) => {
  await page.goto("/");
  await page.getByRole("navigation").getByRole("button", { name: "Workflows", exact: true }).click();
  await page.getByRole("combobox", { name: "Workflow template", exact: true }).selectOption("sop");
  await expect(page.getByTestId("workflow-composer-json")).toHaveValue(/agentd\.sop\.v1/);
  const request = page.waitForRequest((r) => r.url().endsWith("/api/v1/workflow/submit") && r.method() === "POST");
  await page.getByRole("button", { name: "Submit workflow", exact: true }).click();
  await request;
});

test("mobile workspace keeps navigation and composer within the viewport", async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto("/");
  await expect(page.getByLabel("Environment", { exact: true })).toBeVisible();
  await expect(page.getByRole("navigation", { name: "Workspace navigation" })).toBeVisible();
  await expect(page.getByTestId("prompt")).toBeVisible();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  expect(await page.locator("#workspace-main").evaluate((el) => el.scrollWidth <= el.clientWidth)).toBe(true);
  await page.screenshot({ path: "test-results/workspace-mobile.png", fullPage: true });
});
