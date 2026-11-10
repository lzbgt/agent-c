import { expect, test, type Page } from "@playwright/test";

const checkpoint = { checkpoint_path: "private/checkpoint.json", ts_utc: "2026-11-10T00:00:00Z", ts_utc_ms: 100, sha256: "checkpoint-hash", bytes: 200 };
const policy = { include_structured: true, include_daily: true, daily_days: 7, max_items: 12, max_structured_items: 6, max_daily_items: 6, half_life_days: 14, importance_weight: 0.35 };
const queryResult = (prefix = "") => ({
  ok: true, memory_root: "private-memory-root-marker", since_utc_ms: 0, until_utc_ms: 100,
  key_prefix: prefix, limit: 50, returned: 2, checkpoint,
  entries: [
    { key: "project.handover", record: { value: "**Keep the line running.**\n\nCheck the coolant before the next shift.", trace_id: "trace-provenance-marker", versions: [{ value: "old-value-marker" }] } },
    { key: "project.checklist", record: { value: { shift: "Morning", checks: ["Coolant", "Temperature"] }, importance: 0.8 } },
  ],
});

async function mockMemory(page: Page) {
  const calls: { path: string; method: string; params: URLSearchParams; body: any }[] = [];
  await page.addInitScript(() => {
    localStorage.setItem("agentui.connectionMode", JSON.stringify("direct"));
    localStorage.setItem("agentui.base", "http://127.0.0.1:7777");
    localStorage.setItem("agentui.showSettings", "false");
  });
  await page.route("**/api/v1/**", async (route) => {
    const req = route.request();
    const url = new URL(req.url());
    const path = url.pathname;
    const method = req.method();
    const body = method === "POST" ? req.postDataJSON() : null;
    if (path.includes("/memory/") || path.endsWith("/config/update")) calls.push({ path, method, params: url.searchParams, body });
    let response: unknown = { ok: true, service: "agentd", version: "test", sessions: ["default"], messages: [], runs: [], entries: [], tools: [], skills: [], events: [], entities: [], artifacts: [], prefs: {}, features: { client_prefs: { enabled: false } } };
    if (path.endsWith("/memory/query")) {
      const prefix = url.searchParams.get("key_prefix") || "";
      if (prefix === "failure") return route.fulfill({ status: 503, json: { ok: false, error: "Memory storage is unavailable. Try again later." } });
      if (prefix === "invalid") return route.fulfill({ json: { ok: true, entries: "bad-payload" } });
      response = prefix === "empty" ? { ...queryResult(prefix), entries: [], returned: 0 } : queryResult(prefix);
    }
    if (path.endsWith("/memory/correlation/index")) response = { ok: true, memory_root: "private-memory-root-marker", generated_utc_ms: 100, index_path: "correlation.json", token_count: 7, entry_count: 14, structured_entries: 8, daily_entries: 4, recap_entries: 2 };
    if (path.endsWith("/memory/correlate")) response = { ok: true, trace_id: url.searchParams.get("trace_id"), needle: "trace-42", since_utc_ms: 0, until_utc_ms: 100, entries: queryResult().entries };
    if (path.endsWith("/memory/checkpoints")) response = { ok: true, memory_root: "private-memory-root-marker", since_utc_ms: 0, until_utc_ms: 100, checkpoints: [checkpoint] };
    if (path.endsWith("/memory/index")) response = { ok: true, memory_root: "private-memory-root-marker", daily_days: 7, include_structured: true, include_core: true, include_daily: true, include_session: false, files: [{ tier: "core", path: "CORE.md", bytes: 24, lines: 1, token_estimate: 6 }] };
    if (path.endsWith("/memory/salience")) response = { ok: true, memory_root: "private-memory-root-marker", generated_utc_ms: 100, policy, structured_items: [], daily_items: [], returned: 0 };
    if (path.endsWith("/memory/recaps")) response = method === "GET" ? {
      ok: true, memory_root: "private-memory-root-marker", limit: 20, include_summary: true, count: 1,
      recaps: [{ recap_path: "private/recap-path-marker", kind: "daily", ts_utc: "2026-11-10T00:00:00Z", bytes: 321, model: "model-metadata-marker", summary_text: "The morning shift finished all inspections." }],
    } : {
      ok: true, generated_utc_ms: 100, dry_run: body.dry_run, write_file: body.write_file, policy,
      input: { structured_count: 1, daily_count: 0, total_count: 1 },
      structured_items: [{ tier: "structured", key: "project.handover", score: 0.8, text: "Check the coolant before the next shift." }], daily_items: [],
      ...(body.dry_run ? { prompt: "internal-model-prompt-marker" } : { summary_text: "Coolant inspection is ready for the next shift.", ...(body.write_file ? { recap_path: "private/new-recap-path-marker" } : {}) }),
    };
    if (path.endsWith("/config")) response = { ok: true, memory: { recap_daily_interval_ms: 60000, recap_weekly_interval_ms: 120000, recap_daily_days: 2, recap_weekly_days: 9 }, daemon: { summary_model: "summary-model-marker" } };
    if (path.endsWith("/config/update")) response = { ok: true, applied: body };
    if (path.endsWith("/memory/retention/enforce")) response = { ok: true, generated_utc_ms: 100, ...body, daily_deleted_count: 0, checkpoint_deleted_count: 0, structured_deprecated_count: 0 };
    return route.fulfill({ json: response });
  });
  await page.goto("/");
  await page.getByRole("navigation", { name: "Workspace navigation" }).getByRole("button", { name: "Memory", exact: true }).click();
  await expect(page.getByTestId("memory-panel")).toBeVisible();
  return calls;
}

for (const width of [1440, 768, 390, 320]) {
  test(`saved knowledge at ${width}px leads with content and keeps maintenance closed`, async ({ page }, info) => {
    await page.setViewportSize({ width, height: 900 });
    const calls = await mockMemory(page);
    const panel = page.getByTestId("memory-panel");
    const advanced = page.getByTestId("memory-advanced");
    await expect(advanced).not.toHaveAttribute("open", "");
    await expect(page.getByTestId("memory-query-options")).not.toHaveAttribute("open", "");
    await expect(panel.getByRole("button", { name: "Browse memory", exact: true })).toBeInViewport({ ratio: 1 });
    expect(calls).toEqual([]);
    await page.getByTestId("memory-query-prefix-input").fill("project.");
    await page.getByTestId("memory-query-prefix-input").press("Enter");
    await expect(panel.getByText("Check the coolant before the next shift.", { exact: true })).toBeVisible();
    await expect(panel.getByText("Morning", { exact: true })).toBeVisible();
    expect(calls[0].params.get("key_prefix")).toBe("project.");
    expect(await panel.innerText()).not.toMatch(/trace-provenance-marker|private-memory-root-marker|old-value-marker|Structured path|Half-life|Retention/);
    const main = page.locator("#workspace-main");
    expect(await main.evaluate((el) => el.scrollWidth <= el.clientWidth)).toBe(true);
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
    await page.screenshot({ path: info.outputPath(`memory-${width}.png`) });
    await page.getByRole("button", { name: "Summaries", exact: true }).click();
    await page.getByRole("button", { name: "Saved knowledge", exact: true }).click();
    await expect(page.getByTestId("memory-query-prefix-input")).toHaveValue("project.");
    await expect(panel.getByText("Morning", { exact: true })).toBeVisible();
    await panel.locator("article").first().getByText("Record details", { exact: true }).click();
    await expect(panel.locator("article").first().getByText(/trace-provenance-marker/)).toBeVisible();
    expect(calls.filter((call) => call.method !== "GET")).toEqual([]);
  });
}

test("memory empty, service error and malformed responses have clear visible outcomes", async ({ page }) => {
  await mockMemory(page);
  const prefix = page.getByTestId("memory-query-prefix-input");
  const button = page.getByRole("button", { name: "Browse memory", exact: true });
  await prefix.fill("empty"); await button.click();
  await expect(page.getByText("No saved records match this prefix.")).toBeVisible();
  await prefix.fill("failure"); await button.click();
  await expect(page.getByRole("alert")).toHaveText("Memory storage is unavailable. Try again later.");
  await expect(prefix).toHaveValue("failure");
  await prefix.fill("invalid"); await button.click();
  await expect(page.getByRole("alert")).toHaveText("Memory service returned an unexpected response.");
  await expect(page.getByTestId("memory-advanced")).not.toHaveAttribute("open", "");
});

test("summaries show actual text and distinguish preview from generating and saving", async ({ page }) => {
  const calls = await mockMemory(page);
  await page.getByRole("button", { name: "Summaries", exact: true }).click();
  const section = page.getByTestId("memory-recaps-section");
  await expect(section.getByRole("button", { name: "Preview inputs", exact: true })).toBeHidden();
  await section.getByRole("button", { name: "Browse summaries", exact: true }).click();
  await expect(section.getByText("The morning shift finished all inspections.", { exact: true })).toBeVisible();
  expect(calls.at(-1)?.params.get("include_summary")).toBe("true");
  expect(await section.innerText()).not.toMatch(/model-metadata-marker|recap-path-marker/);
  await page.getByTestId("memory-create-summary").locator(":scope > summary").click();
  await section.getByRole("button", { name: "Preview inputs", exact: true }).click();
  await expect(section.getByText("1 source record selected. Nothing saved.")).toBeVisible();
  await expect(section.getByText("Check the coolant before the next shift.", { exact: true })).toBeVisible();
  expect(calls.at(-1)?.body).toMatchObject({ dry_run: true, write_file: true });
  expect(await section.innerText()).not.toMatch(/internal-model-prompt-marker/);
  await section.getByLabel("Preview inputs only", { exact: true }).uncheck();
  await section.getByRole("button", { name: "Create & save summary", exact: true }).click();
  await expect(section.getByText("Summary saved", { exact: true })).toBeVisible();
  await expect(section.getByText("Coolant inspection is ready for the next shift.", { exact: true })).toBeVisible();
  expect(calls.at(-1)?.body).toMatchObject({ dry_run: false, write_file: true });
  await section.getByLabel("Save summary to memory", { exact: true }).uncheck();
  await section.getByRole("button", { name: "Create summary", exact: true }).click();
  await expect(section.getByText("Summary created", { exact: true })).toBeVisible();
  expect(calls.at(-1)?.body).toMatchObject({ dry_run: false, write_file: false });
});

test("advanced trace, index, schedule and cleanup remain available only on request", async ({ page }) => {
  await page.setViewportSize({ width: 320, height: 900 });
  const calls = await mockMemory(page);
  await page.getByTestId("memory-advanced").locator(":scope > summary").click();
  await page.getByText("Trace correlation & checkpoints", { exact: true }).click();
  const diagnostics = page.getByTestId("memory-diagnostics-section");
  await page.getByTestId("memory-trace-id-input").fill("trace-42");
  await diagnostics.getByRole("button", { name: "Build index", exact: true }).click();
  await expect(diagnostics.getByText(/"entry_count": 14/)).toBeVisible();
  await diagnostics.getByRole("button", { name: "Correlate", exact: true }).click();
  await expect(diagnostics.getByText(/"trace_id": "trace-42"/)).toBeVisible();
  await diagnostics.getByRole("button", { name: "List", exact: true }).click();
  await expect(diagnostics.getByText(/checkpoint-hash/)).toBeVisible();
  await page.getByText("Index & relevance tuning", { exact: true }).click();
  const index = page.getByTestId("memory-index-section");
  await index.getByRole("button", { name: "Index", exact: true }).click();
  await expect(index.getByText(/CORE.md/)).toBeVisible();
  await index.getByRole("button", { name: "Fetch", exact: true }).click();
  await expect(index.getByText(/"returned": 0/)).toBeVisible();
  await page.getByText("Retention & cleanup", { exact: true }).click();
  await page.getByRole("button", { name: "Preview cleanup", exact: true }).click();
  await expect(page.getByText("Cleanup preview · nothing changed")).toBeVisible();
  expect(calls.at(-1)?.body).toMatchObject({ dry_run: true, daily_max_days: 30, checkpoint_max_count: 200 });
  const main = page.locator("#workspace-main");
  expect(await main.evaluate((el) => el.scrollWidth <= el.clientWidth)).toBe(true);
  await page.getByRole("button", { name: "Summaries", exact: true }).click();
  await page.getByTestId("memory-summary-settings").locator(":scope > summary").click();
  await page.getByRole("button", { name: "Load config", exact: true }).click();
  await expect(page.getByText(/Summary model: summary-model-marker/)).toBeVisible();
  await page.getByTestId("memory-recap-daily-interval-input").fill("90000");
  await page.getByRole("button", { name: "Apply schedule", exact: true }).click();
  await expect.poll(() => calls.find((call) => call.path.endsWith("/config/update"))?.body).toEqual({ memory: { recap_daily_interval_ms: 90000, recap_weekly_interval_ms: 120000, recap_daily_days: 2, recap_weekly_days: 9 } });
  expect(await main.evaluate((el) => el.scrollWidth <= el.clientWidth)).toBe(true);
});
