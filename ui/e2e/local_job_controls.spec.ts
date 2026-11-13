import fs from "node:fs/promises";
import { expect, test, type Page } from "@playwright/test";

const jobId = "local-output-stop-fixture";
const literalOutput = "zero\tvalue\r\n" + Array.from({ length: 500 }, (_, i) => "entry " + i + ": exact\tvalue").join("\n") + "\nlast: λ\n";
const directoryOutput = "path: /tmp\nentries: 500\nentry-one\nentry-two\n";
const events = [
  { type: "tool_result", data: { tool_name: "shell_exec", tool_call_id: "literal-output", summary: {
    kind: "json_envelope", ok: true, tool: "shell_exec", output: literalOutput,
  } } },
  { type: "tool_result", data: { tool_name: "fs_list", tool_call_id: "directory-listing", summary: {
    kind: "json_envelope", ok: true, tool: "fs_list", output: directoryOutput, truncated: true,
  } } },
];

async function mockActiveLocalJob(page: Page, eventsForJob = events, seedActiveJob = true) {
  const state = {
    status: "running",
    rejectCancel: false,
    cancelCalls: 0,
    progressCalls: 0,
    streamCursors: [] as number[],
    auditRequests: 0,
    auditResponses: 0,
    auditDelayMs: 0,
    runRequests: [] as Record<string, unknown>[],
  };
  await page.addInitScript(({ jobId, seedActiveJob }) => {
    window.__AGENT_UI_CONFIG__ = {
      localModelProfile: true,
      connectionMode: "direct",
      daemonBaseUrl: "http://127.0.0.1:17887",
      maxChars: 321000, maxSteps: 7, maxToolCallsTotal: 13, maxRepeatedToolCalls: 2,
      timeoutMs: 123456,
    };
    if (!window.sessionStorage.getItem("local-job-fixture-seeded")) {
      // Stale browser settings must not override the adapter's local profile.
      for (const field of ["maxChars", "maxSteps", "maxToolCallsTotal", "maxRepeatedToolCalls", "timeoutMs"]) {
        window.localStorage.setItem("agentui." + field, JSON.stringify("999999"));
      }
      window.localStorage.setItem("agentui.maxStepsUserSet", "true");
      if (seedActiveJob) {
        // Only the cursor survives a reload; retained events must be replayed.
        window.localStorage.setItem("agentui.jobsBySession", JSON.stringify(JSON.stringify({
          default: { job_id: jobId, cursor: 2, started_unix_ms: Date.now() },
        })));
      }
      window.sessionStorage.setItem("local-job-fixture-seeded", "true");
    }
  }, { jobId, seedActiveJob });

  const headers = {
    "access-control-allow-origin": "*",
    "access-control-allow-methods": "GET,POST,OPTIONS",
    "access-control-allow-headers": "authorization,content-type",
  };
  await page.route("**/api/v1/**", async (route) => {
    const url = new URL(route.request().url());
    const json = (body: unknown, status = 200) => route.fulfill({ status, headers, json: body });
    if (route.request().method() === "OPTIONS") {
      await route.fulfill({ status: 204, headers });
      return;
    }
    if (url.pathname === "/api/v1/run_async") {
      state.runRequests.push(route.request().postDataJSON());
      await json({ ok: true, job_id: jobId });
      return;
    }
    if (url.pathname === "/api/v1/job/stream") {
      const cursor = Number(url.searchParams.get("cursor") || 0);
      state.streamCursors.push(cursor);
      // End the stream after the retained events to exercise polling reconnect.
      await route.fulfill({
        headers,
        contentType: "text/event-stream",
        body: eventsForJob.slice(cursor).map((event, index) =>
          "id: " + (cursor + index) + "\nevent: agent_event\ndata: " + JSON.stringify(event) + "\n\n").join(""),
      });
      return;
    }
    if (url.pathname === "/api/v1/job/cancel") {
      state.cancelCalls += 1;
      await json(state.rejectCancel ? { ok: false, error: "fixture rejection" } : { ok: true }, state.rejectCancel ? 409 : 200);
      return;
    }
    if (url.pathname === "/api/v1/job") {
      const includeEvents = url.searchParams.get("include_events") === "1";
      if (includeEvents) state.progressCalls += 1;
      const cursor = Number(url.searchParams.get("cursor") || 0);
      await json({
        ok: true, job_id: jobId, status: state.status, updated_unix_ms: Date.now(),
        ...(includeEvents ? { events: eventsForJob.slice(cursor), events_cursor_next: eventsForJob.length } : {}),
        ...(state.status === "cancelled" ? { result: { ok: false, error: "cancelled", events: eventsForJob } } : {}),
        ...(state.status === "done" ? { result: { ok: true, assistant_text: "See captured tool output.", events: eventsForJob } } : {}),
      });
      return;
    }
    if (url.pathname === "/api/v1/session/audit") {
      state.auditRequests += 1;
      const completed = state.status === "done" && url.searchParams.get("session_id") === "default";
      if (completed && state.auditDelayMs) await new Promise((resolve) => setTimeout(resolve, state.auditDelayMs));
      await json({ ok: true, entries: completed ? [{
        ts_unix_ms: Date.now(), prompt: "Show exact output", assistant_text: "See captured tool output.",
        ok: true, events: eventsForJob,
      }] : [] });
      state.auditResponses += 1;
      return;
    }
    if (url.pathname === "/api/v1/health") {
      await json({ ok: true, service: "agentd", version: "test", ready: true });
      return;
    }
    if (url.pathname === "/api/v1/sessions") {
      await json({ ok: true, sessions: ["default", "other"] });
      return;
    }
    if (url.pathname === "/api/v1/session") {
      await json({ ok: true, session_id: "default", messages: [] });
      return;
    }
    await json({ ok: true, entries: [], messages: [], runs: [], artifacts: [] });
  });
  return state;
}

test("local run limits come from adapter config despite stale browser settings", async ({ page }) => {
  const state = await mockActiveLocalJob(page, events, false);
  await page.goto("/");
  await page.getByTestId("prompt").fill("Check the configured local profile.");
  await expect(page.getByTestId("run")).toBeEnabled();
  await page.getByTestId("run").click();
  await expect.poll(() => state.runRequests.length).toBe(1);
  expect(state.runRequests[0]).toMatchObject({
    max_chars: 321000, max_steps: 7, max_tool_calls_total: 13,
    max_repeated_tool_calls: 2, timeout_ms: 123456,
  });
});

test("active local jobs replay output after reload and keep Stop pending through reconnect", async ({ page, context }) => {
  test.setTimeout(45_000);
  const state = await mockActiveLocalJob(page);
  await context.grantPermissions(["clipboard-read", "clipboard-write"]);
  await page.goto("/");
  const stop = page.getByTestId("stop-run");
  await expect(stop).toBeVisible();
  await expect(stop).toBeInViewport();
  await expect.poll(() => state.streamCursors[0]).toBe(0);
  await expect(page.getByTestId("chat-tool-outputs")).toBeVisible();
  await expect.poll(() => state.progressCalls).toBeGreaterThan(0);
  await expect.poll(async () => page.evaluate(() => {
    const stored = JSON.parse(JSON.parse(localStorage.getItem("agentui.jobsBySession") || '"{}"'));
    return Object.values(stored).some((job: any) => job.cursor === 2);
  })).toBe(true);

  await page.reload();
  await expect(stop).toBeVisible();
  await expect.poll(() => state.streamCursors.length).toBeGreaterThan(1);
  expect(state.streamCursors.at(-1)).toBe(0);
  const outputs = page.getByTestId("chat-tool-outputs");
  await expect(outputs).toContainText("Tool outputs (2)");
  await outputs.locator("summary").first().click();
  await expect(outputs.getByText("Captured command output", { exact: true })).toBeVisible();
  await expect(outputs.getByText("Directory listing (fs_list)", { exact: true })).toBeVisible();
  await expect(outputs.getByTestId("tool-output-truncation")).toContainText("truncated");
  await expect(page.getByTestId("conversation-details")).not.toHaveAttribute("open", "");

  await outputs.getByTestId("tool-output-copy").first().click();
  expect(await page.evaluate(() => navigator.clipboard.readText())).toBe(literalOutput);
  const downloadPromise = page.waitForEvent("download");
  await outputs.getByTestId("tool-output-download").first().click();
  const download = await downloadPromise;
  expect(download.suggestedFilename()).toBe("shell_exec.txt");
  expect(await fs.readFile((await download.path())!, "utf8")).toBe(literalOutput);

  await stop.click();
  await expect(stop).toHaveText("Stopping…");
  await expect(stop).toBeDisabled();
  const progressAtCancel = state.progressCalls;
  await expect.poll(() => state.progressCalls).toBeGreaterThan(progressAtCancel + 1);
  await expect(page.getByText("Stopping current run…", { exact: true })).toBeVisible();
  expect(state.cancelCalls).toBe(1);
  state.status = "cancelled";
  await expect(stop).toHaveCount(0);
  await expect(page.getByText("Stopping current run…", { exact: true })).toHaveCount(0);
});

test("rejected cancellation remains visible and permits a retry", async ({ page }) => {
  test.setTimeout(30_000);
  const state = await mockActiveLocalJob(page);
  state.rejectCancel = true;
  await page.goto("/");
  const stop = page.getByTestId("stop-run");
  await expect(stop).toBeVisible();
  await stop.click();
  await expect(page.getByText("Stop failed: fixture rejection", { exact: true })).toBeVisible();
  await expect(stop).toBeEnabled();
  await expect(stop).toHaveText("Stop");
  const progressAfterReject = state.progressCalls;
  await expect.poll(() => state.progressCalls).toBeGreaterThan(progressAfterReject + 1);
  await expect(page.getByText("Stop failed: fixture rejection", { exact: true })).toBeVisible();
  state.rejectCancel = false;
  await stop.click();
  await expect(stop).toHaveText("Stopping…");
  await expect(page.getByText("Stop failed: fixture rejection", { exact: true })).toHaveCount(0);
  expect(state.cancelCalls).toBe(2);
  state.status = "cancelled";
  await expect(stop).toHaveCount(0);
});

test("completed runs keep opened tool outputs through audit handover and reload", async ({ page, context }) => {
  test.setTimeout(35_000);
  const state = await mockActiveLocalJob(page);
  await context.grantPermissions(["clipboard-read", "clipboard-write"]);
  await page.goto("/");
  const outputs = page.getByTestId("chat-tool-outputs");
  await expect(outputs).toBeVisible();
  await outputs.locator("summary").first().click();
  const command = outputs.locator("section").filter({ has: page.getByRole("heading", { name: "shell_exec", exact: true }) });
  await expect(command.getByTestId("tool-output-copy")).toBeVisible();

  // The native job finishes before its refreshed audit arrives.
  state.auditDelayMs = 2500;
  const auditBeforeCompletion = state.auditRequests;
  state.status = "done";
  await expect(page.getByTestId("stop-run")).toHaveCount(0);
  await expect.poll(() => state.auditRequests).toBeGreaterThan(auditBeforeCompletion);
  await expect(outputs).toHaveAttribute("open", "", { timeout: 3000 });
  await expect(command.getByTestId("tool-output-copy")).toBeVisible();
  await command.getByTestId("tool-output-copy").click();
  expect(await page.evaluate(() => navigator.clipboard.readText())).toBe(literalOutput);

  await expect.poll(() => state.auditResponses).toBe(state.auditRequests);
  await expect(outputs).toHaveAttribute("open", "");
  await command.getByTestId("tool-output-copy").click();
  expect(await page.evaluate(() => navigator.clipboard.readText())).toBe(literalOutput);

  // Retaining a completed run must not leak its output into another session.
  await page.locator("#local-chat-session").selectOption("other");
  await expect(outputs).toHaveCount(0);
  await page.locator("#local-chat-session").selectOption("default");
  await expect(outputs).toBeVisible();

  await page.reload();
  await expect(page.getByTestId("stop-run")).toHaveCount(0);
  await expect(outputs).toBeVisible();
  await outputs.locator("summary").first().click();
  await command.getByTestId("tool-output-copy").click();
  expect(await page.evaluate(() => navigator.clipboard.readText())).toBe(literalOutput);
  const downloadPromise = page.waitForEvent("download");
  await command.getByTestId("tool-output-download").click();
  const download = await downloadPromise;
  expect(await fs.readFile((await download.path())!, "utf8")).toBe(literalOutput);
});

test("display clipping is distinct from complete native capture", async ({ page, context }) => {
  test.setTimeout(20_000);
  const clippedEvents = [{
    ...events[0],
    data: {
      ...events[0].data,
      summary: {
        ...events[0].data.summary, output: "visible\n",
        captured_output_bytes: 1000, observed_output_bytes: 1000,
        capture_complete: true, capture_truncated: false, output_display_truncated: true,
      },
    },
  }];
  await mockActiveLocalJob(page, clippedEvents);
  await context.grantPermissions(["clipboard-read", "clipboard-write"]);
  await page.goto("/");
  const outputs = page.getByTestId("chat-tool-outputs");
  await expect(outputs).toBeVisible();
  await outputs.locator("summary").first().click();
  await expect(outputs.getByTestId("tool-output-size")).toHaveText("1000 captured bytes; 8 displayed");
  await expect(outputs.getByTestId("tool-output-truncation")).toContainText("displayed excerpt");
  await outputs.getByTestId("tool-output-copy").click();
  expect(await page.evaluate(() => navigator.clipboard.readText())).toBe("visible\n");
});
