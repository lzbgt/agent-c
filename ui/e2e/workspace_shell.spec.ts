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
    if (path.endsWith("/workflows")) {
      return route.fulfill({ json: { ok: true, workflows: [{
        workflow_id: "wf_89cf09b3-ba91-4a18-bead-9381a6a81b45", status: "running",
        trace_id: "inspection-process-production-line-one", session_id: "inspection-shift-morning",
        idempotency_key: "inspection-line-one-november-10-morning", updated_unix_ms: 1700000100000,
      }] } });
    }
    await route.fulfill({ json: { ok: true, service: "agentd", version: "test", features: {}, sessions: [], tools: [], messages: [], runs: [], events: [], entities: [], skills: [], workflows: [], schedules: [], approvals: [], prefs: {} } });
  });
});

test("workspace navigation retains accessible names, active state and return path", async ({ page }) => {
  await page.goto("/");
  const nav = page.getByRole("navigation", { name: "Workspace navigation" });
  await expect(nav.getByRole("button", { name: "Chat", exact: true })).toHaveAttribute("aria-current", "page");
  await expect(page.getByRole("heading", { name: "Conversation", exact: true })).toBeVisible();
  await expect(page.getByTestId("workspace-empty")).toBeVisible();
  await page.screenshot({ path: "test-results/workspace-desktop.png", fullPage: true });
  await nav.getByRole("button", { name: "Workflows", exact: true }).click();
  await expect(page.getByRole("heading", { name: "Workflows", exact: true })).toBeVisible();
  await expect(nav.getByRole("button", { name: "Workflows", exact: true })).toHaveAttribute("aria-current", "page");
  await expect(page.getByTestId("workspace-empty")).toHaveCount(0);
  await expect(page.getByTestId("promptbar-root")).toBeHidden();
  await expect(page.getByTestId("workflow-list-panel")).toBeVisible();
  await expect(page.getByText("Workflow · a6a81b45", { exact: true })).toBeVisible();
  await page.getByRole("button", { name: "Collapse tools", exact: true }).click();
  await nav.getByRole("button", { name: "Chat", exact: true }).click();
  await expect(page.getByTestId("workspace-empty")).toBeVisible();
  await page.getByRole("button", { name: "Start a conversation" }).click();
  await expect(page.getByTestId("prompt")).toBeFocused();
});

test("SOP authoring template reaches the existing submit API", async ({ page }) => {
  await page.goto("/");
  await page.getByRole("navigation").getByRole("button", { name: "Workflows", exact: true }).click();
  await page.getByRole("button", { name: "Create workflow", exact: true }).click();
  await page.getByRole("combobox", { name: "Workflow template", exact: true }).selectOption("sop");
  await expect(page.getByTestId("workflow-composer-json")).toHaveValue(/agentd\.sop\.v1/);
  const request = page.waitForRequest((r) => r.url().endsWith("/api/v1/workflow/submit") && r.method() === "POST");
  await page.getByRole("button", { name: "Submit workflow", exact: true }).click();
  await request;
});

for (const size of [{ width: 320, height: 740 }, { width: 390, height: 844 }, { width: 768, height: 1024 }]) {
test(`responsive workspace at ${size.width}px keeps tools primary and preserves the composer draft`, async ({ page }) => {
  await page.setViewportSize(size);
  await page.goto("/");
  await expect(page.getByRole("button", { name: "Settings", exact: true })).toBeVisible();
  await expect(page.getByRole("navigation", { name: "Workspace navigation" })).toBeVisible();
  await expect(page.getByTestId("prompt")).toBeVisible();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  expect(await page.locator("#workspace-main").evaluate((el) => el.scrollWidth <= el.clientWidth)).toBe(true);
  await page.getByTestId("prompt").fill("Prepare the inspection workflow");
  const nav = page.getByRole("navigation", { name: "Workspace navigation" });
  await nav.getByRole("button", { name: "Workflows", exact: true }).click();
  await expect(page.getByTestId("promptbar-root")).toBeHidden();
  await expect(page.getByTestId("workflow-list-panel")).toBeVisible();
  const list = await page.getByTestId("workflow-list-panel").boundingBox();
  expect(list!.y).toBeLessThan(size.height * .65);
  expect(await page.locator("#workspace-main").evaluate((el) => el.scrollWidth <= el.clientWidth)).toBe(true);
  await page.screenshot({ path: `test-results/workflows-${size.width}.png`, fullPage: true });
  await page.getByRole("button", { name: "Create workflow", exact: true }).click();
  await page.getByRole("combobox", { name: "Workflow template", exact: true }).selectOption("sop");
  await expect(page.getByTestId("workflow-composer-json")).toHaveValue(/agentd\.sop\.v1/);
  expect(await page.locator("#workspace-main").evaluate((el) => el.scrollWidth <= el.clientWidth)).toBe(true);
  await page.screenshot({ path: `test-results/workflow-create-${size.width}.png`, fullPage: true });
  // Moving between sections keeps authoring state while changing the primary task.
  await page.getByRole("button", { name: "Runs", exact: true }).click();
  await page.getByRole("button", { name: "Create workflow", exact: true }).click();
  await expect(page.getByTestId("workflow-composer-json")).toHaveValue(/agentd\.sop\.v1/);
  for (const name of ["Memory", "Trace", "Run Diff", "Approvals", "Voice"]) {
    await nav.getByRole("button", { name, exact: true }).click();
    await expect(page.getByTestId("workspace-tool")).toBeVisible();
    await expect(nav.getByRole("button", { name, exact: true })).toBeInViewport();
    expect(await page.locator("#workspace-main").evaluate((el) => el.scrollWidth <= el.clientWidth)).toBe(true);
  }
  await nav.getByRole("button", { name: "Chat", exact: true }).click();
  await expect(page.getByTestId("prompt")).toHaveValue("Prepare the inspection workflow");
  const main = await page.locator("#workspace-main").boundingBox();
  const composer = await page.getByTestId("promptbar-root").boundingBox();
  expect(main!.y + main!.height).toBeLessThanOrEqual(composer!.y + 1);
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  const dialog = page.getByRole("dialog", { name: "Settings", exact: true });
  await expect(dialog).toBeVisible();
  expect(await dialog.evaluate((el) => el.scrollWidth <= el.clientWidth)).toBe(true);
  // Native modal dialog keeps keyboard focus inside and restores the invoking control.
  await expect(page.getByTestId("settings-close")).toBeFocused();
  await page.keyboard.press("Shift+Tab");
  expect(await dialog.evaluate((el) => el.contains(document.activeElement))).toBe(true);
  await page.keyboard.press("Escape");
  await expect(dialog).toHaveCount(0);
  await expect(page.getByRole("button", { name: "Settings", exact: true })).toBeFocused();
  await page.screenshot({ path: `test-results/workspace-${size.width}.png`, fullPage: true });
});
}

test("composer stays reachable when the mobile viewport shrinks for a keyboard", async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto("/");
  await page.getByTestId("prompt").focus();
  await page.setViewportSize({ width: 390, height: 420 });
  await expect(page.getByTestId("prompt")).toBeInViewport({ ratio: 1 });
  await expect(page.getByTestId("run")).toBeInViewport({ ratio: 1 });
  expect(await page.evaluate(() => document.documentElement.scrollHeight <= innerHeight)).toBe(true);
});
