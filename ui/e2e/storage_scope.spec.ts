import { expect, test } from "@playwright/test";

test("switching storage scopes loads the selected queue and fences stale callbacks", async ({ page }) => {
  await page.route("**/api/v1/**", route => route.fulfill({ json: { ok: true, sessions: [], features: {}, tools: [], prefs: {} } }));
  await page.goto("/");
  await page.evaluate(async () => {
    localStorage.setItem("scope-test.first", JSON.stringify(["first queued request"]));
    localStorage.setItem("scope-test.second", JSON.stringify(["second queued request"]));
    const module = "/e2e/storageScopeHarness.tsx";
    (await import(module)).mount();
  });
  await expect.poll(() => page.evaluate(() => (window as any).storageScopeTest?.queue)).toEqual(["first queued request"]);
  await page.evaluate(() => (window as any).storageScopeTest.setScope("second"));
  await expect.poll(() => page.evaluate(() => (window as any).storageScopeTest?.queue)).toEqual(["second queued request"]);
  await page.evaluate(() => (window as any).storageScopeTest.setters.first((queue: string[]) => [...queue, "late first callback"]));
  await page.waitForTimeout(100);
  expect(await page.evaluate(() => (window as any).storageScopeTest.queue)).toEqual(["second queued request"]);
  expect(await page.evaluate(() => JSON.parse(localStorage.getItem("scope-test.second")!))).toEqual(["second queued request"]);
  await page.evaluate(() => (window as any).storageScopeTest.setters.second((queue: string[]) => [...queue, "new second request"]));
  await expect.poll(() => page.evaluate(() => JSON.parse(localStorage.getItem("scope-test.second")!))).toEqual(["second queued request", "new second request"]);
  await page.evaluate(() => (window as any).storageScopeTest.setScope("first"));
  await expect.poll(() => page.evaluate(() => (window as any).storageScopeTest?.queue)).toEqual(["first queued request"]);
  await page.evaluate(() => (window as any).storageScopeTest.setScope("missing"));
  await expect.poll(() => page.evaluate(() => (window as any).storageScopeTest?.queue)).toEqual([]);
});
