import { expect, test } from "@playwright/test";
test("typed paging client enforces generation and cursor protocol", async ({ page }) => {
  await page.route("**/api/v1/**", route => {
    const url = new URL(route.request().url());
    if (url.pathname.endsWith("/session")) {
      const revision = url.searchParams.get("messages_revision");
      if (revision === "9") return route.fulfill({ status: 409, json: { ok: false, error: "session revision changed" } });
      return route.fulfill({ json: { ok: true, session_id: "history", messages: [
        { index: 3, role: "user", content: "three" }, { index: 4, role: "assistant", content: "four" },
      ], page: { revision: 10, has_more: true, next_before_index: 3 } } });
    }
    return route.fulfill({ json: { ok: true, sessions: [], features: {}, tools: [], prefs: {} } });
  });
  await page.goto("/");
  const result = await page.evaluate(async () => {
    const modulePath = "/src/api/sessionPages.ts";
    const { apiGetSessionPage } = await import(modulePath);
    const first = await apiGetSessionPage("", "history", { limit: 2 });
    const attempt = async (options: object) => {
      try { await apiGetSessionPage("", "history", options); return "unexpected success"; }
      catch (error: any) { return { name: error.name, status: error.status || 0, message: error.message }; }
    };
    return { first, stale: await attempt({ limit: 2, beforeIndex: 3, revision: 9 }),
      missing: await attempt({ beforeIndex: 3 }), changed: await attempt({ revision: 8 }),
      unordered: await attempt({ beforeIndex: 4, revision: 10 }) };
  });
  expect(result.first.messages.map((v: any) => v.index)).toEqual([3, 4]);
  expect(result.stale.status).toBe(409);
  expect(result.missing.message).toContain("Invalid session page cursor");
  expect(result.changed.status).toBe(409);
  expect(result.unordered.message).toContain("ordering");
});
