import { expect, test, type Page } from "@playwright/test";
async function mount(page: Page) {
  await page.evaluate(async () => {
    const path = "/e2e/chatFenceHarness.tsx";
    (await import(path)).mount();
  });
}
test("uncertain queued chat is never automatically reposted, including after reload", async ({ page }) => {
  let posts = 0;
  await page.route("**/api/v1/**", route => route.fulfill({ json: { ok: true, sessions: [], features: {}, tools: [], prefs: {} } }));
  await page.route("**/api/v1/run_async", route => { posts++; return route.abort("failed"); });
  await page.goto("/");
  await mount(page);
  await expect.poll(() => page.evaluate(() => (window as any).chatFenceTest?.prompt)).toBe("first request");
  expect(posts).toBe(1);
  const state = await page.evaluate(() => (window as any).chatFenceTest);
  expect(state.queue.map((v: any) => v.prompt)).toEqual(["second request"]);
  expect(JSON.parse(state.pending).trace_id).toMatch(/^trace_/);
  expect(state.pending).not.toContain("first request");
  await page.reload(); await mount(page);
  await expect.poll(() => page.evaluate(() => Boolean((window as any).chatFenceTest?.pending))).toBe(true);
  await page.waitForTimeout(300);
  expect(posts).toBe(1);
});
test("unavailable receipt storage sends zero chat POSTs", async ({ page }) => {
  let posts = 0;
  await page.route("**/api/v1/**", route => route.fulfill({ json: { ok: true, sessions: [], features: {}, tools: [], prefs: {} } }));
  await page.route("**/api/v1/run_async", route => { posts++; return route.abort(); });
  await page.goto("/");
  await page.evaluate(() => {
    const original = Storage.prototype.setItem;
    Storage.prototype.setItem = function(key, value) {
      if (key.startsWith("agentui.pendingChatSubmission.")) throw new Error("quota");
      return original.call(this, key, value);
    };
  });
  await mount(page);
  await expect.poll(() => page.evaluate(() => (window as any).chatFenceTest?.prompt)).toBe("first request");
  expect(posts).toBe(0);
  expect((await page.evaluate(() => (window as any).chatFenceTest)).queue).toHaveLength(1);
});

test("accepted jobs clear only their fence and ordinary queued work continues", async ({ page }) => {
  let posts = 0;
  await page.route("**/api/v1/**", route => route.fulfill({ json: { ok: true, sessions: [], features: {}, tools: [], prefs: {} } }));
  await page.route("**/api/v1/run_async", route => { posts++; return route.fulfill({ json: { ok: true, job_id: "job-" + posts } }); });
  await page.goto("/"); await mount(page);
  await expect.poll(() => posts).toBe(2);
  await expect.poll(() => page.evaluate(() => (window as any).chatFenceTest?.pending)).toBe(null);
});
