import { expect, test, type Page } from "@playwright/test";

async function mount(page: Page, options: { async?: boolean; strict?: boolean; emptyQueue?: boolean } = {}) {
  await page.evaluate(async options => {
    const module = "/e2e/chatFenceHarness.tsx";
    (await import(module)).mount(options);
  }, options);
}
async function state(page: Page) {
  return page.evaluate(() => (window as any).chatFenceTest);
}
async function setup(page: Page) {
  await page.route("**/api/v1/**", route => route.fulfill({ json: { ok: true, sessions: [], features: {}, tools: [], prefs: {} } }));
  await page.goto("/");
}
function receipt(trace: string, extra = {}) {
  return { ok: true, job_id: "original-job", trace_id: trace, session_id: "fence-test", status: "running", durable_only: true, ...extra };
}

test("lost acceptance automatically reattaches the unique original job without replay", async ({ page }) => {
  let posts = 0, lookups = 0, trace = "";
  await setup(page);
  await page.route("**/api/v1/run_async", route => {
    posts++;trace = route.request().postDataJSON().trace_id;
    return route.abort("failed");
  });
  await page.route("**/api/v1/job?**", route => {
    lookups++;
    const url = new URL(route.request().url());
    expect(url.searchParams.get("trace_id")).toBe(trace);
    expect(url.searchParams.get("session_id")).toBe("fence-test");
    expect(route.request().method()).toBe("GET");
    return route.fulfill({ json: receipt(trace) });
  });
  await mount(page);
  await expect.poll(async () => (await state(page))?.active).toBe("original-job");
  await expect.poll(async () => (await state(page))?.pending).toBe(null);
  expect(posts).toBe(1);expect(lookups).toBe(1);
  expect((await state(page)).queue.map((item: any) => item.prompt)).toEqual(["second request"]);
  const watches = await page.evaluate(() => JSON.parse(JSON.parse(localStorage.getItem("agentui.jobsBySession")!)));
  expect(watches["fence-test-scope"].job_id).toBe("original-job");
});

test("bounded probes tolerate admission visibility delay, then resume watching", async ({ page }) => {
  let posts = 0, lookups = 0, trace = "";
  await setup(page);
  await page.route("**/api/v1/run_async", route => { posts++;trace = route.request().postDataJSON().trace_id;return route.abort(); });
  await page.route("**/api/v1/job?**", route => {
    lookups++;
    return lookups < 3 ? route.fulfill({ status: 404, json: { ok: false } }) : route.fulfill({ json: receipt(trace) });
  });
  await mount(page);
  await expect.poll(async () => (await state(page))?.active).toBe("original-job");
  expect(posts).toBe(1);expect(lookups).toBe(3);
});

test("current acceptance retains a watch even when the cache is full of future timestamps", async ({ page }) => {
  let posts = 0;
  await setup(page);
  await page.evaluate(() => {
    const watches = Object.fromEntries(Array.from({ length: 80 }, (_, i) => ["other-scope-" + i, {
      job_id: "older-job-" + i, cursor: 0, started_unix_ms: Date.now() + 86400000 + i,
    }]));
    localStorage.setItem("agentui.jobsBySession", JSON.stringify(JSON.stringify(watches)));
  });
  await page.route("**/api/v1/run_async", route => {
    posts++;
    return route.fulfill({ json: { ok: true, job_id: "original-job", trace_id: route.request().postDataJSON().trace_id } });
  });
  await mount(page);
  await expect.poll(async () => (await state(page))?.active).toBe("original-job");
  expect((await state(page)).pending).toBe(null);
  const watches = await page.evaluate(() => JSON.parse(JSON.parse(localStorage.getItem("agentui.jobsBySession")!)));
  expect(watches["fence-test-scope"].job_id).toBe("original-job");
  expect(Object.keys(watches).length).toBeLessThanOrEqual(64);
  expect(posts).toBe(1);
});

test("missing receipts exhaust three reads and retain the hold across unrelated renders", async ({ page }) => {
  let posts = 0, lookups = 0;
  await setup(page);
  await page.route("**/api/v1/run_async", route => { posts++;return route.abort(); });
  await page.route("**/api/v1/job?**", route => { lookups++;return route.fulfill({ status: 404, json: { ok: false } }); });
  await mount(page);
  await expect.poll(() => lookups).toBe(3);
  await expect.poll(async () => (await state(page))?.checking).toBe(false);
  await page.waitForTimeout(400);
  expect(lookups).toBe(3);expect(posts).toBe(1);
  expect((await state(page)).pending).toBeTruthy();
  expect((await state(page)).queue).toHaveLength(1);
  await page.evaluate(() => (window as any).chatFenceTest.recover());
  expect(lookups).toBe(4);expect(posts).toBe(1);
});

for (const failure of ["identity", "state", "ambiguous", "auth", "oversized", "invalid-json"] as const) {
  test(`unsafe ${failure} lookup remains held without repeated probes`, async ({ page }) => {
    let posts = 0, lookups = 0, trace = "";
    await setup(page);
    await page.route("**/api/v1/run_async", route => { posts++;trace = route.request().postDataJSON().trace_id;return route.abort(); });
    await page.route("**/api/v1/job?**", route => {
      lookups++;
      if (failure === "auth" || failure === "ambiguous") return route.fulfill({ status: failure === "auth" ? 401 : 409, json: { ok: false } });
      if (failure === "oversized") return route.fulfill({ contentType: "application/json", body: " ".repeat(65537) });
      if (failure === "invalid-json") return route.fulfill({ contentType: "application/json", body: "not JSON" });
      return route.fulfill({ json: receipt(trace, failure === "identity" ? { session_id: "another-session" } : { status: "invented" }) });
    });
    await mount(page);
    await expect.poll(async () => (await state(page))?.notice?.includes("remains held")).toBe(true);
    await page.waitForTimeout(300);
    expect(lookups).toBe(1);expect(posts).toBe(1);
    expect((await state(page)).active).toBe(null);
    expect((await state(page)).pending).toBeTruthy();
  });
}

test("reload in StrictMode recovers the saved receipt without submitting queued text", async ({ page }) => {
  let posts = 0, lookups = 0;
  await setup(page);
  await page.route("**/api/v1/run_async", route => { posts++;return route.abort(); });
  await page.route("**/api/v1/job?**", route => { lookups++;return route.fulfill({ json: receipt("saved-trace") }); });
  await page.evaluate(() => localStorage.setItem("agentui.pendingChatSubmission.fence-test-scope", JSON.stringify({
    version: 1, mode: "async", trace_id: "saved-trace", session_id: "fence-test", created_unix_ms: Date.now(),
  })));
  await page.reload();await mount(page, { strict: true });
  await expect.poll(async () => (await state(page))?.active).toBe("original-job");
  expect(posts).toBe(0);
  expect(lookups).toBeGreaterThanOrEqual(1);
  expect(lookups).toBeLessThanOrEqual(2); // first StrictMode probe may be aborted
  expect((await state(page)).queue).toHaveLength(2);
});

for (const change of ["session", "auth"] as const) {
  test(`late receipt cannot attach after ${change} changes`, async ({ page }) => {
    let lookups = 0, release!: () => void;
    const gate = new Promise<void>(resolve => { release = resolve; });
    await setup(page);
    await page.route("**/api/v1/run_async", route => route.abort());
    await page.route("**/api/v1/job?**", async route => {
      const trace = new URL(route.request().url()).searchParams.get("trace_id")!;
      lookups++;
      if (lookups === 1) { await gate;await route.fulfill({ json: receipt(trace) }).catch(() => {}); }
      else await route.fulfill({ status: 401, json: { ok: false } });
    });
    await mount(page);
    await expect.poll(() => lookups).toBe(1);
    await page.evaluate(change => (window as any).chatFenceTest.changeContext(change === "session"
      ? { scope: "other-scope", session: "other-session" } : { auth: { mode: "direct", token: "fixture-other-principal" } }), change);
    release();
    if (change === "auth") await expect.poll(() => lookups).toBe(2);
    await page.waitForTimeout(300);
    expect((await state(page)).active).toBe(null);
    const held = await page.evaluate(() => localStorage.getItem("agentui.pendingChatSubmission.fence-test-scope"));
    expect(held).toBeTruthy();
    expect(held).not.toContain("fixture-other-principal");
  });
}

test("watch persistence failure keeps the receipt until read-only recovery can save it", async ({ page }) => {
  let posts = 0, trace = "";
  await setup(page);
  await page.evaluate(() => {
    const original = Storage.prototype.setItem;
    (window as any).failWatchWrite = true;
    Storage.prototype.setItem = function(key, value) {
      if ((window as any).failWatchWrite && key === "agentui.jobsBySession") throw new Error("fixture quota");
      return original.call(this, key, value);
    };
  });
  await page.route("**/api/v1/run_async", route => {
    posts++;trace = route.request().postDataJSON().trace_id;
    return route.fulfill({ json: { ok: true, job_id: "original-job", trace_id: trace } });
  });
  await page.route("**/api/v1/job?**", route => route.fulfill({ json: receipt(trace) }));
  await mount(page);
  await expect.poll(async () => (await state(page))?.notice?.includes("remains held")).toBe(true);
  expect((await state(page)).pending).toBeTruthy();expect(posts).toBe(1);
  await page.evaluate(() => { (window as any).failWatchWrite = false; });
  await page.evaluate(() => (window as any).chatFenceTest.recover());
  await expect.poll(async () => (await state(page))?.active).toBe("original-job");
  expect((await state(page)).pending).toBe(null);expect(posts).toBe(1);
});

test("sync uncertainty and corrupt receipts never trigger automatic job adoption", async ({ page }) => {
  let posts = 0, lookups = 0;
  await setup(page);
  await page.route("**/api/v1/run", route => { posts++;return route.abort(); });
  await page.route("**/api/v1/job?**", route => { lookups++;return route.fulfill({ json: receipt("wrong") }); });
  await mount(page, { async: false });
  await expect.poll(async () => (await state(page))?.prompt).toBe("first request");
  await page.waitForTimeout(300);expect(posts).toBe(1);expect(lookups).toBe(0);
  await page.evaluate(() => localStorage.setItem("agentui.pendingChatSubmission.fence-test-scope", "corrupt"));
  await page.reload();await mount(page);
  await page.waitForTimeout(300);expect(posts).toBe(1);expect(lookups).toBe(0);
  expect((await state(page)).pending).toBe("corrupt");
});
