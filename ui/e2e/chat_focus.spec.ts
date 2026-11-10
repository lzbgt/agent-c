import { expect, test } from "@playwright/test";

const greeting = "Hello! How can I help you today?";
const turn = (time: number, trace: string) => ({
  ts_unix_ms: time, trace_id: trace, prompt: "hello", assistant_text: greeting, ok: true, model: "model-metadata-marker", tools: "host",
  events: [
    { type: "start", trace_id: trace, data: { trace_id: trace } },
    { type: "tool_call", data: { tool_name: "shell_exec", arguments_json: '{"cmd":"metadata-command-marker"}' } },
    { type: "tool_result", data: { tool_name: "shell_exec", content: "tool-metadata-marker" } },
    { type: "assistant_delta", data: { delta: "Hello! " } },
    { type: "assistant_message", data: { assistant_content: greeting } },
  ],
});

test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => {
    localStorage.setItem("agentui.connectionMode", JSON.stringify("direct"));
    localStorage.setItem("agentui.base", "http://127.0.0.1:7777");
    localStorage.setItem("agentui.showSettings", "false");
    // Existing debug preferences must never expand the default conversation.
    localStorage.setItem("agentui.showDebugInConversation", "true");
  });
});

for (const width of [1440, 390, 320]) {
  test(`a greeting at ${width}px shows messages once and groups metadata out of view`, async ({ page }) => {
    let turns = [turn(300, "trace-hello-1")];
    let snapshot = 300;
    await page.setViewportSize({ width, height: 900 });
    await page.route("**/api/v1/**", async (route) => {
      const path = new URL(route.request().url()).pathname;
      if (path.endsWith("/session/audit")) return route.fulfill({ json: { ok: true, entries: turns } });
      if (path.endsWith("/db/messages")) return route.fulfill({ json: { ok: true, messages: [
        { role: "system", content: "system-metadata-marker", created_unix_ms: snapshot },
        ...turns.flatMap(() => [{ role: "user", content: "hello", created_unix_ms: snapshot }, { role: "assistant", content: greeting, created_unix_ms: snapshot }]),
      ] } });
      return route.fulfill({ json: { ok: true, service: "agentd", version: "test", sessions: ["default"], messages: [], runs: [], entries: [], events: [], tools: [], skills: [], entities: [], artifacts: [], prefs: {}, features: { client_prefs: { enabled: false } } } });
    });
    await page.goto("/");
    const chat = page.getByRole("log", { name: "Conversation", exact: true });
    await expect(chat.getByTestId("chat-user")).toHaveText(/Youhello/);
    await expect(chat.getByTestId("chat-assistant")).toContainText(greeting);
    await expect(chat.getByText(greeting, { exact: true })).toHaveCount(1);
    await expect(page.getByTestId("conversation-details")).not.toHaveAttribute("open", "");
    const visible = await page.locator("body").innerText();
    expect(visible).not.toMatch(/system-metadata-marker|model-metadata-marker|tool-metadata-marker|Latest outcome|Technical history|No artifacts|items|events/);
    await expect(page.getByTestId("promptbar-details")).toHaveCount(0);
    await page.screenshot({ path: `test-results/chat-greeting-${width}.png` });

    // Repeated identical turns are real messages, never content-deduplicated.
    turns = [...turns, turn(400, "trace-hello-2")]; snapshot = 400;
    await page.reload();
    await expect(chat.getByTestId("chat-user")).toHaveCount(2);
    await expect(chat.getByTestId("chat-assistant")).toHaveCount(2);
    await page.getByTestId("conversation-details").locator(":scope > summary").click();
    await expect(page.getByText("Stored context", { exact: true })).toBeVisible();
    await expect(page.getByText("Technical history", { exact: true })).toBeVisible();
  });
}

test("settings leads with subject options and keeps technical controls closed", async ({ page }) => {
  const requests: string[] = [];
  await page.route("**/api/v1/**", async (route) => {
    requests.push(new URL(route.request().url()).pathname);
    await route.fulfill({ json: { ok: true, service: "agentd", version: "test", sessions: ["default"], messages: [], runs: [], entries: [], events: [], tools: [], skills: [], entities: [], prefs: {}, features: { client_prefs: { enabled: false } } } });
  });
  await page.goto("/");
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  const dialog = page.getByRole("dialog", { name: "Settings", exact: true });
  await expect(dialog.getByRole("textbox", { name: "Model", exact: true })).toBeVisible();
  await expect(dialog.getByRole("combobox", { name: "Agent capabilities" })).toBeVisible();
  await expect(dialog.getByLabel("Conversation", { exact: true })).toBeVisible();
  await expect(page.getByTestId("settings-advanced")).not.toHaveAttribute("open", "");
  expect(await dialog.innerText()).not.toMatch(/Lease|YOLO|Proxy URL|Summary max chars|Server profile sync|API key/);
  expect(requests.some((path) => path.endsWith("/diagnostics"))).toBe(false);
  await dialog.getByRole("combobox", { name: "Agent capabilities" }).selectOption("none");
  await page.screenshot({ path: "test-results/chat-settings.png" });
  await page.getByTestId("settings-advanced").locator(":scope > summary").click();
  await dialog.getByText("Execution & providers", { exact: true }).click();
  await expect(dialog.getByText("API key (local)", { exact: true })).toBeVisible();
  await page.keyboard.press("Escape");
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  await expect(page.getByTestId("settings-advanced")).not.toHaveAttribute("open", "");
  await expect(dialog.getByRole("combobox", { name: "Agent capabilities" })).toHaveValue("none");
});

test("streaming and persisted replies have one visible identity while identical turns remain distinct", async ({ page }) => {
  await page.goto("/");
  const result = await page.evaluate(async () => {
    const { buildChatTranscript } = await import("/src/history/chatTranscript.ts");
    const live = { ts_unix_ms: 200, prompt: "hello", live: true, job_id: "job-new", trace_id: "trace-new", events: [
      { type: "assistant_delta", data: { delta: "Hel" } }, { type: "assistant_delta", data: { delta: "lo" } },
    ] };
    const partial = buildChatTranscript([], [live]);
    const settled = { ...live, live: false, assistant_text: "Hello", events: [...live.events, { type: "assistant_message", data: { assistant_content: "Hello" } }] };
    const complete = buildChatTranscript([], [settled, live]);
    const repeated = buildChatTranscript([], [{ ...settled, ts_unix_ms: 100, trace_id: "trace-old" }, settled]);
    const newerSnapshot = [{ role: "system", content: "hidden", created_unix_ms: 300 }, { role: "user", content: "new request", created_unix_ms: 300 }, { role: "assistant", content: "new reply", created_unix_ms: 300 }];
    const fresh = buildChatTranscript(newerSnapshot, [settled]);
    const echo = buildChatTranscript([], [{ ts_unix_ms: 1, prompt: "echo", assistant_text: "echo", events: [] }]);
    return { partial: partial.map((item) => item.content), complete: complete.map((item) => item.content), repeated: repeated.length, fresh: fresh.map((item) => item.content), echo: echo.length };
  });
  expect(result).toEqual({ partial: ["hello", "Hello"], complete: ["hello", "Hello"], repeated: 4, fresh: ["new request", "new reply"], echo: 2 });
});

test("automatic browser actions execute while activity details remain collapsed", async ({ page }) => {
  await page.addInitScript(() => { localStorage.setItem("agentui.allowClientRpcs", "true"); localStorage.setItem("agentui.allowClientEffects", "true"); });
  const results: any[] = [];
  await page.route("**/api/v1/**", async (route) => {
    const path = new URL(route.request().url()).pathname;
    if (path.endsWith("/session/scene/apply")) return route.fulfill({ json: { ok: true, updated_unix_ms: 500, scene: {
      "note-result": { id: "note-result", kind: "note", title: "Result", props: { text: "Autonomous UI result" }, created_ms: 500, updated_ms: 500 },
    } } });
    if (path.endsWith("/session/audit")) return route.fulfill({ json: { ok: true, entries: [{
      ts_unix_ms: 1, prompt: "Create a note", live: true, job_id: "job-live-ui", events: [
        { type: "ui_action", data: { tool_call_id: "tc-note", action: { type: "client_rpc", rpc_id: "rpc-note", auto_run: true,
          rpc: { kind: "entity_apply", args: { ops: [{ op: "create", id: "note-result", entity_kind: "note", title: "Result", props: { text: "Autonomous UI result" } }] } } } } },
        { type: "assistant_message", data: { assistant_content: "Your note is ready." } },
      ],
    }] } });
    if (path.endsWith("/session/client_event") && route.request().method() === "POST") results.push(route.request().postDataJSON());
    return route.fulfill({ json: { ok: true, service: "agentd", version: "test", sessions: [], messages: [], runs: [], events: [], entities: [], artifacts: [], skills: [], prefs: {}, features: { client_prefs: { enabled: false } } } });
  });
  await page.goto("/");
  await expect(page.getByRole("log", { name: "Conversation", exact: true })).toContainText("Your note is ready.");
  await expect.poll(() => results.filter((item) => item.type === "client_rpc_result" && item.data?.rpc_id === "rpc-note" && item.data?.ok === true).length).toBe(1);
  await expect(page.getByTestId("conversation-details")).not.toHaveAttribute("open", "");
  await expect(page.getByText("Autonomous UI result", { exact: true })).toBeVisible();
});

test("sending a message clears only the accepted draft and respects IME composition", async ({ page }) => {
  await page.addInitScript(() => { localStorage.setItem("agentui.useAsync", "false"); });
  const requests: any[] = [];
  let entries: any[] = [];
  let releaseReply = () => {};
  let rejectNext = false;
  await page.route("**/api/v1/**", async (route) => {
    const path = new URL(route.request().url()).pathname;
    if (path.endsWith("/run") && route.request().method() === "POST") {
      const body = route.request().postDataJSON(); requests.push(body);
      if (rejectNext) return route.fulfill({ json: { ok: false, error: "Provider unavailable" } });
      if (requests.length === 2) await new Promise<void>((resolve) => { releaseReply = resolve; });
      entries = [...entries, { ...turn(500 + requests.length, body.trace_id), prompt: body.prompt }];
      return route.fulfill({ json: { ok: true, assistant_text: greeting } });
    }
    if (path.endsWith("/session/audit")) return route.fulfill({ json: { ok: true, entries } });
    return route.fulfill({ json: { ok: true, service: "agentd", version: "test", sessions: [], messages: [], runs: [], events: [], artifacts: [], entities: [], skills: [], prefs: {}, features: { jobs: { enabled: false }, client_prefs: { enabled: false } } } });
  });
  await page.goto("/");
  const prompt = page.getByTestId("prompt");
  await expect(page.getByTestId("run")).toBeDisabled();
  await prompt.fill("你好");
  await prompt.evaluate((el) => el.dispatchEvent(new KeyboardEvent("keydown", { key: "Enter", bubbles: true, isComposing: true })));
  expect(requests).toHaveLength(0);
  await prompt.press("Control+Enter");
  await expect(page.getByRole("log", { name: "Conversation", exact: true })).toContainText(greeting);
  await expect(prompt).toHaveValue("");
  expect(requests).toHaveLength(1);
  expect(requests[0].prompt).toBe("你好");
  await prompt.fill("second request");
  await page.getByTestId("run").click();
  await expect.poll(() => requests.length).toBe(2);
  await prompt.fill("next draft");
  releaseReply();
  await expect(page.getByTestId("chat-assistant")).toHaveCount(2);
  await expect(prompt).toHaveValue("next draft");
  rejectNext = true;
  await page.getByTestId("run").click();
  await expect(page.getByRole("alert")).toContainText("Provider unavailable");
  await expect(prompt).toHaveValue("next draft");
  await expect(page.getByTestId("promptbar-details")).toHaveCount(0);
});
