import fs from "node:fs";
import { expect, test } from "@playwright/test";

// Opt-in production qualification. Use an isolated browser context and never
// record HTTP headers/cookies. This operator probe does not prove owner OAuth.
const configPath = process.env.AGENTC_LIVE_CONFIG;
test.skip(!configPath, "Set AGENTC_LIVE_CONFIG to the private Arch gateway config");
test.use({ trace: "off", video: "off", screenshot: "off" });

for (const viewport of [
  { width: 1440, height: 1000 },
  { width: 320, height: 740 },
  { width: 390, height: 844 },
  { width: 768, height: 1024 },
]) {
  test(`live workspace and workflows at ${viewport.width}px`, async ({ page }, info) => {
    const cfg = JSON.parse(fs.readFileSync(configPath!, "utf8"));
    const origin = new URL(cfg.public_url).origin;
    const executionWrites: string[] = [];
    await page.setViewportSize(viewport);
    await page.route(`${origin}/**`, async (route) => {
      const { pathname } = new URL(route.request().url());
      if (route.request().method() !== "GET" && /^\/api\/v1\/(run(?:_async)?|workflow\/submit)$/.test(pathname)) {
        executionWrites.push(pathname);
        return route.fulfill({ status: 403, json: { error: "layout_probe_refuses_execution" } });
      }
      if (pathname.startsWith("/api/") || pathname === "/auth/session") {
        return route.continue({ headers: { ...route.request().headers(), Authorization: `Bearer ${cfg.operator_token}` } });
      }
      return route.continue();
    });
    await page.goto(origin, { waitUntil: "domcontentloaded" });
    await expect(page.getByRole("status", { name: "Connected", exact: true })).toBeVisible();
    const nav = page.getByRole("navigation", { name: "Workspace navigation" });
    const main = page.locator("#workspace-main");
    await expect(page.getByRole("button", { name: "Sign out of workspace" })).toBeInViewport({ ratio: 1 });
    expect(await main.evaluate((el) => el.scrollWidth <= el.clientWidth)).toBe(true);
    const activity = page.getByTestId("conversation-details");
    if (await activity.count()) await expect(activity).not.toHaveAttribute("open", "");
    await page.screenshot({ path: info.outputPath("workspace.png") });
    const presentation = page.locator('iframe[data-testid="scene-script-frame"]').first();
    if (await presentation.count()) {
      const canvas = presentation.contentFrame().locator("canvas");
      if (await canvas.count()) {
        await expect(canvas).toBeVisible();
        const runtime = await presentation.evaluate((frame: HTMLIFrameElement) => frame.contentDocument?.body.dataset.sceneRuntime);
        const hash = () => canvas.evaluate(async (element: HTMLCanvasElement) => {
          const bytes = await (await fetch(element.toDataURL())).arrayBuffer();
          return Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes))).map((n) => n.toString(16).padStart(2, "0")).join("");
        });
        const first = await hash();
        await page.getByTestId("prompt").pressSequentially("local draft probe");
        if (process.env.AGENTC_LIVE_EXPECT_ANIMATION === "1") await expect.poll(() => hash()).not.toBe(first);
        expect(await presentation.evaluate((frame: HTMLIFrameElement) => frame.contentDocument?.body.dataset.sceneRuntime)).toBe(runtime);
        await page.getByTestId("prompt").fill("");
      }
    }

    await nav.getByRole("button", { name: "Workflows", exact: true }).click();
    await expect(page.getByRole("group", { name: "Workflow sections" })).toBeInViewport({ ratio: 1 });
    await expect(page.getByTestId("workflow-list-panel")).toBeVisible();
    await expect(page.getByTestId("promptbar-root")).toBeHidden();
    await expect(page.getByTestId("workspace-empty")).toHaveCount(0);
    expect(await main.evaluate((el) => el.scrollWidth <= el.clientWidth)).toBe(true);
    await page.screenshot({ path: info.outputPath("workflow-runs.png") });

    await page.getByRole("button", { name: "Create workflow", exact: true }).click();
    await page.getByRole("combobox", { name: "Workflow template", exact: true }).selectOption("sop");
    await expect(page.getByTestId("workflow-composer-json")).toHaveValue(/agentd\.sop\.v1/);
    expect(await main.evaluate((el) => el.scrollWidth <= el.clientWidth)).toBe(true);
    await page.screenshot({ path: info.outputPath("workflow-create.png") });

    await page.getByRole("button", { name: "Settings", exact: true }).click();
    const dialog = page.getByRole("dialog", { name: "Settings", exact: true });
    await expect(dialog).toBeVisible();
    await expect(dialog.getByLabel("Model", { exact: true })).toBeVisible();
    await expect(page.getByTestId("settings-advanced")).not.toHaveAttribute("open", "");
    expect(await dialog.evaluate((el) => el.scrollWidth <= el.clientWidth)).toBe(true);
    await page.keyboard.press("Escape");
    await expect(dialog).toHaveCount(0);
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
    expect(executionWrites).toEqual([]);
  });
}
