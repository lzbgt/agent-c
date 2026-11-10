import { expect, test } from "@playwright/test";

for (const width of [1440, 768, 390, 320]) {
  test(`tool pages at ${width}px keep actions and output primary`, async ({ page }, info) => {
    const unexpectedCalls: string[] = [];
    await page.setViewportSize({ width, height: 900 });
    await page.addInitScript(() => {
      localStorage.setItem("agentui.connectionMode", JSON.stringify("direct"));
      localStorage.setItem("agentui.base", "http://127.0.0.1:7777");
      localStorage.setItem("agentui.showSettings", "false");
    });
    await page.route("**/api/v1/**", async (route) => {
      const req = route.request();
      const url = new URL(req.url());
      // Client presence advertisements are separate from executing work.
      if ((req.method() === "POST" && !url.pathname.endsWith("/session/client_event")) || url.pathname.endsWith("/voice_stats")) unexpectedCalls.push(url.pathname);
      if (url.pathname.endsWith("/trace")) return route.fulfill({ json: url.searchParams.get("trace_id") === "failure" ? { ok: false, error: "Trace storage is unavailable." } : {
        ok: true, trace_id: "trace-tool-test", records: [
          { ts_unix_ms: 100, session_id: "session-metadata-marker", trace_id: "trace-tool-test", ok: true, prompt: "Inspect the line", assistant_text: "The line is ready for the next shift.", events: [{ type: "tool_call", data: { tool_name: "shell_exec", arguments_json: "tool-command-marker" } }] },
          { ts_unix_ms: 200, ok: false, prompt: "Inspect the furnace", error: "Furnace sensor is unavailable." },
        ],
      } });
      return route.fulfill({ json: { ok: true, service: "agentd", version: "test", sessions: ["default"], messages: [], runs: [], entries: [], events: [], tools: [], skills: [], entities: [], artifacts: [], prefs: {}, features: { client_prefs: { enabled: false } } } });
    });
    await page.route("**/voice/v1/config", (route) => route.fulfill({ json: { available: true, max_session_seconds: 300 } }));
    await page.goto("/");
    const nav = page.getByRole("navigation", { name: "Workspace navigation" });
    const main = page.locator("#workspace-main");
    await nav.getByRole("button", { name: "Voice", exact: true }).click();
    await expect(page.getByTestId("voice-call-start")).toBeInViewport({ ratio: 1 });
    await expect(page.getByTestId("voice-call-end")).toHaveCount(0);
    expect(await main.evaluate((el) => el.scrollWidth <= el.clientWidth)).toBe(true);
    await page.screenshot({ path: info.outputPath("voice.png") });
    await nav.getByRole("button", { name: "Run Diff", exact: true }).click();
    await expect(page.getByLabel("Run A", { exact: true })).toBeVisible();
    await expect(page.getByTestId("run-diff-details")).not.toHaveAttribute("open", "");
    expect(await main.innerText()).not.toMatch(/request diffs|tool records|usage delta|replay hash/);
    expect(await main.evaluate((el) => el.scrollWidth <= el.clientWidth)).toBe(true);
    await page.screenshot({ path: info.outputPath("run-comparison.png") });
    await nav.getByRole("button", { name: "Trace", exact: true }).click();
    await page.getByLabel("Trace ID", { exact: true }).fill("trace-tool-test");
    await page.getByRole("button", { name: "Load", exact: true }).click();
    await expect(page.getByText("The line is ready for the next shift.", { exact: true })).toBeVisible();
    await expect(page.getByText("Furnace sensor is unavailable.", { exact: true })).toBeVisible();
    expect(await main.innerText()).not.toMatch(/session-metadata-marker|tool-command-marker|shell_exec/);
    expect(await main.evaluate((el) => el.scrollWidth <= el.clientWidth)).toBe(true);
    await page.screenshot({ path: info.outputPath("trace-output.png") });
    const output = page.getByTestId("trace-output").filter({ hasText: "The line is ready" });
    await output.getByText("Record details", { exact: true }).click();
    await expect(output.getByText(/session-metadata-marker/)).toBeVisible();
    await page.getByLabel("Trace ID", { exact: true }).fill("failure");
    await page.getByRole("button", { name: "Load", exact: true }).click();
    await expect(page.getByText("Trace storage is unavailable.", { exact: true })).toBeVisible();
    await expect(page.getByTestId("trace-results")).toHaveCount(0);
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
    expect(unexpectedCalls).toEqual([]);
  });
}
