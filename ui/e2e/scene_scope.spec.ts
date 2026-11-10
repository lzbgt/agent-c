import { expect, test, type Page } from "@playwright/test";

const script = `const state = window.__probe = { boot: crypto.randomUUID(), keys: 0, frames: 0, ticks: 0 };
api.root.dataset.boot = state.boot;
api.root.dataset.epoch = String(api.props.epoch || 1);
document.addEventListener('keydown', () => { state.keys++; api.root.dataset.keys = String(state.keys); });
window.addEventListener('keydown', () => { state.keys++; api.root.dataset.keys = String(state.keys); });
setInterval(() => { state.ticks++; }, 20);
function tick(){ state.frames++; api.root.dataset.frames = String(state.frames); requestAnimationFrame(tick); }
requestAnimationFrame(tick);`;

async function workspace(page: Page, kind: "canvas2d" | "dom") {
  let epoch = 1;
  let reads = 0;
  const prompts: string[] = [];
  await page.addInitScript(() => {
    localStorage.setItem("agentui.connectionMode", JSON.stringify("direct"));
    localStorage.setItem("agentui.base", "http://127.0.0.1:7777");
    localStorage.setItem("agentui.showSettings", "false");
  });
  await page.route("**/api/v1/**", async (route) => {
    const req = route.request();
    const path = new URL(req.url()).pathname;
    if (path.endsWith("/session/scene")) {
      reads++;
      return route.fulfill({ json: { ok: true, session_id: "default", updated_unix_ms: epoch, scene: Object.fromEntries(["a", "b"].map((id, i) => [id, {
        id, kind, title: `Presentation ${id.toUpperCase()}`, created_ms: 100 - i, updated_ms: 100 - i,
        props: { width: 480, height: 160, epoch, script: kind === "dom" ? `async api => { ${script} }` : script,
          ...(kind === "dom" ? { html: '<style>body{background:rgb(1,2,3)}</style><label>Local field<input aria-label="Local field"></label>' } : {}),
        },
      }])) } });
    }
    if (req.method() === "POST" && /\/(run|run_async)$/.test(path)) {
      prompts.push(req.postDataJSON().prompt);
      return route.fulfill({ json: { ok: true, session_id: "default", assistant_text: "Accepted." } });
    }
    return route.fulfill({ json: { ok: true, service: "agentd", version: "test", sessions: ["default"], messages: [], runs: [], entries: [], events: [], tools: [], skills: [], entities: [], prefs: {}, features: { jobs: { enabled: false }, client_prefs: { enabled: false } } } });
  });
  await page.goto("/");
  return { prompts, reads: () => reads, update: () => { epoch++; } };
}

for (const width of [1440, 390]) {
  test(`canvas lifetime and keyboard focus stay local at ${width}px`, async ({ page }, info) => {
    await page.setViewportSize({ width, height: 1000 });
    const state = await workspace(page, "canvas2d");
    const a = page.frameLocator('iframe[title="Presentation A"]');
    const canvasA = a.locator("canvas");
    await expect(canvasA).toHaveAttribute("data-boot", /.+/);
    const boot = await canvasA.getAttribute("data-boot");
    const frames = Number(await canvasA.getAttribute("data-frames"));
    const prompt = page.getByTestId("prompt");
    await page.evaluate(() => {
      (window as any).__workspaceKeys = 0;
      for (const type of ["keydown", "keyup", "keypress"]) window.addEventListener(type, () => { (window as any).__workspaceKeys++; });
    });
    await prompt.pressSequentially("draft stays local");
    expect(await page.evaluate(() => (window as any).__workspaceKeys)).toBe(0);
    await expect(canvasA).toHaveAttribute("data-boot", boot!);
    await expect(canvasA).not.toHaveAttribute("data-keys", /.+/);
    await expect.poll(async () => Number(await canvasA.getAttribute("data-frames"))).toBeGreaterThan(frames);
    await page.getByTestId("scene-history-toggle").click();
    await page.getByTestId("scene-entity-b").getByRole("button", { name: "Presentation B", exact: true }).click();
    const canvasB = page.frameLocator('iframe[title="Presentation B"]').locator("canvas");
    await expect(canvasB).toHaveAttribute("data-boot", /.+/);
    await canvasA.click(); await page.keyboard.press("ArrowRight");
    await expect(canvasA).toHaveAttribute("data-keys", "2");
    await expect(canvasB).not.toHaveAttribute("data-keys", /.+/);
    await page.keyboard.press("Enter");
    expect(state.prompts).toEqual([]);
    await expect(prompt).toHaveValue("draft stays local");
    await canvasB.click(); await page.keyboard.press("ArrowLeft");
    await expect(canvasB).toHaveAttribute("data-keys", "2");
    await expect(canvasA).toHaveAttribute("data-keys", "4");
    await prompt.focus(); await page.keyboard.press("Escape");
    await prompt.press("Control+Enter");
    await expect.poll(() => state.prompts).toEqual(["draft stays local"]);
    await expect(canvasA).toHaveAttribute("data-boot", boot!);
    // Polling returns new object identities with identical inputs.
    await expect.poll(state.reads).toBeGreaterThan(1);
    await expect(canvasA).toHaveAttribute("data-boot", boot!);
    state.update();
    await expect(canvasA).toHaveAttribute("data-epoch", "2");
    await expect(canvasA).not.toHaveAttribute("data-boot", boot!);
    // Even scripts that return no cleanup lose their clocks when detached.
    await page.locator('iframe[title="Presentation A"]').evaluate((frame: HTMLIFrameElement) => { (window as any).__oldPresentation = (frame.contentWindow as any).__probe; });
    await page.getByTestId("scene-entity-a").getByRole("button", { name: "Presentation A", exact: true }).click();
    await expect(page.locator('iframe[title="Presentation A"]')).toHaveCount(0);
    const stopped = await page.evaluate(() => ({ ...(window as any).__oldPresentation }));
    await page.waitForTimeout(150);
    expect(await page.evaluate(() => ({ ...(window as any).__oldPresentation }))).toEqual(stopped);
    expect(await page.locator("#workspace-main").evaluate((el) => el.scrollWidth <= el.clientWidth)).toBe(true);
    await prompt.fill("draft survives loading the fixed workspace");
    await page.reload();
    await expect(prompt).toHaveValue("draft survives loading the fixed workspace");
    await page.screenshot({ path: info.outputPath(`canvas-scope-${width}.png`) });
  });
}

test("DOM presentation styles, input and handlers cannot capture the composer", async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 844 });
  const state = await workspace(page, "dom");
  const frame = page.frameLocator('iframe[title="Presentation A"]');
  const local = frame.getByLabel("Local field", { exact: true });
  await expect(local).toBeVisible();
  await local.pressSequentially("inside");
  await expect(local).toHaveValue("inside");
  const keys = await frame.locator("#scene-root").getAttribute("data-keys");
  await page.getByTestId("prompt").pressSequentially("outside");
  await expect(frame.locator("#scene-root")).toHaveAttribute("data-keys", keys!);
  await expect(local).toHaveValue("inside");
  expect(await page.locator("body").evaluate((el) => getComputedStyle(el).backgroundColor)).not.toBe("rgb(1, 2, 3)");
  await local.press("Control+Enter");
  expect(state.prompts).toEqual([]);
  await expect(page.getByTestId("prompt")).toHaveValue("outside");
});
