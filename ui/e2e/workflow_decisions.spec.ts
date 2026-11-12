import { expect, test, type Page } from "@playwright/test";

function decision(workflowId = "wf-review", state = "waiting") {
  return {
    workflow_id: workflowId, task_id: "review", decision_id: "wd-" + workflowId, request_hash: "sha256:fixture",
    prompt: workflowId === "wf-next" ? "Accept the next inspection?" : "Release this inspected batch?",
    choices: [{ id: "release", label: "Release batch", outcome: "continue" }, { id: "reject", label: "Reject batch", outcome: "fail" }],
    state, available: state === "waiting", created_unix_ms: 1700000000000,
  };
}

async function setup(page: Page, read: (workflowId: string, port: string) => Record<string, unknown>) {
  await page.addInitScript(() => {
    localStorage.setItem("agentui.connectionMode", JSON.stringify("direct"));
    localStorage.setItem("agentui.base", "http://127.0.0.1:7777");
    localStorage.setItem("agentui.showSettings", "false");
  });
  await page.route("**/api/v1/**", (route) => route.fulfill({ json: {
    ok: true, service: "agentd", version: "test", features: {}, sessions: [], tools: [], messages: [],
    runs: [], events: [], entities: [], skills: [], workflows: [], schedules: [], approvals: [], prefs: {},
  } }));
  await page.route("**/api/v1/workflows**", (route) => route.fulfill({ json: { ok: true,
    workflows: ["wf-review", "wf-next"].map((workflow_id) => ({ workflow_id, status: "running", cancel_requested: false })),
  } }));
  await page.route(/\/api\/v1\/workflow\?/, (route) => {
    const url = new URL(route.request().url());
    const workflowId = url.searchParams.get("workflow_id")!;
    return route.fulfill({ json: { ok: true, workflow: { workflow_id: workflowId, status: "running", cancel_requested: false },
      tasks: [{ task_id: "review", status: "queued", attempt: 0, max_attempts: 1, depends_on: [] }],
      ...read(workflowId, url.port),
    } });
  });
}

async function open(page: Page, workflowId = "wf-review") {
  await page.goto("/");
  await page.getByRole("navigation").getByRole("button", { name: "Workflows", exact: true }).click();
  await page.getByTestId("workflow-list-panel").getByText(workflowId, { exact: true }).click();
  await expect(page.getByTestId("workflow-decisions")).toBeVisible();
}

test("configured choices record exactly one explicit answer with a readable mobile receipt", async ({ page }) => {
  let saved: Record<string, unknown> | undefined;
  const requests: Record<string, unknown>[] = [];
  await setup(page, (id) => ({ decisions: [saved ?? decision(id)] }));
  let release!: () => void;
  const gate = new Promise<void>((resolve) => { release = resolve; });
  await page.route("**/api/v1/workflow/decision", async (route) => {
    const body = route.request().postDataJSON(); requests.push(body);
    await gate;
    saved = { ...decision(), state: "resolved", available: false, choice_id: body.choice_id, outcome: "continue", note: body.note };
    return route.fulfill({ json: { ok: true, replayed: false, decision: saved } });
  });
  await open(page);
  await page.setViewportSize({ width: 320, height: 740 });
  const section = page.getByTestId("workflow-decisions");
  await expect(section).toContainText("does not grant extra tool permissions");
  expect(await section.evaluate((element) => element.scrollWidth <= element.clientWidth)).toBe(true);
  await section.screenshot({ path: "test-results/workflow-decision-mobile.png" });
  await page.getByRole("textbox", { name: "Decision note for review" }).fill("Evidence checked");
  const choice = section.getByRole("button", { name: /Release batch/ });
  await choice.evaluate((element: HTMLButtonElement) => { element.click(); element.click(); });
  await expect(section).toContainText("Recording choice");
  await expect(choice).toBeDisabled();
  await expect.poll(() => requests.length).toBe(1);
  expect(requests[0]).toMatchObject({ workflow_id: "wf-review", task_id: "review", decision_id: "wd-wf-review", choice_id: "release", note: "Evidence checked" });
  expect(Object.keys(requests[0]).sort()).toEqual(["choice_id", "decision_id", "idempotency_key", "note", "task_id", "workflow_id"]);
  expect(requests[0].idempotency_key).toMatch(/^ui-/);
  release();
  await expect(section).toContainText("Continue outcome recorded");
  await expect(section).toContainText("Evidence checked");
  await expect(section.getByRole("button")).toHaveCount(0);
});

test("a lost response preserves the exact choice, note and delivery key on retry", async ({ page }) => {
  const requests: Record<string, unknown>[] = [];
  await setup(page, (id) => ({ decisions: [decision(id)] }));
  await page.route("**/api/v1/workflow/decision", async (route) => {
    const body = route.request().postDataJSON(); requests.push(body);
    if (requests.length === 1) return route.abort("failed");
    return route.fulfill({ json: { ok: true, replayed: true, decision: {
      ...decision(), state: "resolved", available: false, choice_id: "reject", outcome: "fail", note: body.note,
    } } });
  });
  await open(page);
  await page.getByRole("textbox", { name: "Decision note for review" }).fill("Inspection failed");
  const section = page.getByTestId("workflow-decisions");
  await section.getByRole("button", { name: /Reject batch/ }).click();
  await expect(section.getByRole("alert")).toContainText("No different choice will be sent");
  await expect(page.getByRole("textbox", { name: "Decision note for review" })).toBeDisabled();
  await expect(page.getByRole("textbox", { name: "Decision note for review" })).toHaveValue("Inspection failed");
  await expect(section.getByRole("button", { name: /Release batch/ })).toHaveCount(0);
  await section.getByRole("button", { name: "Retry same choice: Reject batch", exact: true }).click();
  await expect(section).toContainText("Fail outcome recorded");
  expect(requests).toHaveLength(2);
  expect(requests[1]).toEqual(requests[0]);
});

for (const state of ["stopped", "expired", "owner-cancelled"]) {
  test(`${state} decisions cannot send a choice`, async ({ page }) => {
    let posts = 0;
    await setup(page, (id) => ({
      decisions: [decision(id, state === "owner-cancelled" ? "waiting" : state)],
      ...(state === "owner-cancelled" ? { workflow: { workflow_id: id, status: "running", cancel_requested: true } } : {}),
    }));
    await page.route("**/api/v1/workflow/decision", (route) => { posts++; return route.fulfill({ json: { ok: true } }); });
    await open(page);
    await expect(page.getByTestId("workflow-decisions").getByRole("button")).toHaveCount(0);
    await expect(page.getByTestId("workflow-decisions")).toContainText(state === "expired" ? "Decision expired" : "Workflow stopped");
    expect(posts).toBe(0);
  });
}

test("a late choice response cannot replace the newly selected workflow", async ({ page }) => {
  await setup(page, (id) => ({ decisions: [decision(id)] }));
  let requested = false, release!: () => void;
  const gate = new Promise<void>((resolve) => { release = resolve; });
  await page.route("**/api/v1/workflow/decision", async (route) => {
    requested = true; await gate;
    return route.fulfill({ json: { ok: true, replayed: false, decision: {
      ...decision(), prompt: "Old private receipt", state: "resolved", available: false, choice_id: "release", outcome: "continue",
    } } }).catch(() => {});
  });
  await open(page);
  await page.getByTestId("workflow-decisions").getByRole("button", { name: /Release batch/ }).click();
  await expect.poll(() => requested).toBe(true);
  await page.getByTestId("workflow-list-panel").getByText("wf-next", { exact: true }).click();
  await expect(page.getByTestId("workflow-decisions")).toContainText("Accept the next inspection?");
  release();
  await expect(page.getByTestId("workflow-decisions")).not.toContainText("Old private receipt");
  await expect(page.getByTestId("workflow-decisions").getByRole("button", { name: /Release batch/ })).toBeEnabled();
});

test("connection changes discard in-flight receipt UI and do not retarget a choice", async ({ page }) => {
  const posts: string[] = [];
  await setup(page, (id, port) => ({ decisions: [{ ...decision(id), prompt: port === "8888" ? "New connection review" : "Old connection review" }] }));
  let release!: () => void;
  const gate = new Promise<void>((resolve) => { release = resolve; });
  await page.route("**/api/v1/workflow/decision", async (route) => {
    posts.push(new URL(route.request().url()).port);
    await gate;
    return route.fulfill({ json: { ok: true, replayed: false, decision: {
      ...decision(), prompt: "Old private receipt", state: "resolved", available: false, choice_id: "release", outcome: "continue",
    } } }).catch(() => {});
  });
  await open(page);
  await page.getByTestId("workflow-decisions").getByRole("button", { name: /Release batch/ }).click();
  await expect.poll(() => posts.length).toBe(1);
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  await page.getByTestId("settings-advanced").locator(":scope > summary").click();
  await page.getByText("Connections & access", { exact: true }).click();
  await page.getByTestId("daemon-base").fill("http://127.0.0.1:8888");
  await page.getByTestId("settings-close").click();
  await page.getByTestId("workflow-list-panel").getByText("wf-review", { exact: true }).click();
  await expect(page.getByTestId("workflow-decisions")).toContainText("New connection review");
  release();
  await expect(page.getByTestId("workflow-decisions")).not.toContainText("Old private receipt");
  expect(posts).toEqual(["7777"]);
});

test("late workflow lookups cannot replace the current actionable decision", async ({ page }) => {
  await setup(page, (id) => ({ decisions: [decision(id)] }));
  let requested = false, release!: () => void;
  const gate = new Promise<void>((resolve) => { release = resolve; });
  await page.route(/\/api\/v1\/workflow\?workflow_id=wf-review.*/, async (route) => {
    requested = true; await gate;
    return route.fulfill({ json: { ok: true, workflow: { workflow_id: "wf-review", status: "running" },
      tasks: [], decisions: [{ ...decision(), prompt: "Stale selected decision" }] } }).catch(() => {});
  });
  await page.goto("/");
  await page.getByRole("navigation").getByRole("button", { name: "Workflows", exact: true }).click();
  await page.getByTestId("workflow-list-panel").getByText("wf-review", { exact: true }).click();
  await expect.poll(() => requested).toBe(true);
  await page.getByTestId("workflow-list-panel").getByText("wf-next", { exact: true }).click();
  await expect(page.getByTestId("workflow-decisions")).toContainText("Accept the next inspection?");
  release();
  await page.waitForTimeout(200);
  await expect(page.getByTestId("workflow-decisions")).not.toContainText("Stale selected decision");
});

test("active detail refresh discovers a newly activated review without occupying a UI action", async ({ page }) => {
  let activated = false;
  await setup(page, (id) => ({ decisions: activated ? [decision(id)] : [] }));
  await page.goto("/");
  await page.getByRole("navigation").getByRole("button", { name: "Workflows", exact: true }).click();
  await page.getByTestId("workflow-list-panel").getByText("wf-review", { exact: true }).click();
  await expect(page.getByTestId("workflow-detail-panel")).toBeVisible();
  activated = true;
  await expect(page.getByTestId("workflow-decisions")).toBeVisible({ timeout: 10000 });
});

test("review template is provider-free and never submits an implicit answer or run", async ({ page }) => {
  let submits = 0, choices = 0;
  await setup(page, (id) => ({ decisions: [decision(id)] }));
  await page.route("**/api/v1/workflow/decision", (route) => { choices++; return route.fulfill({ json: { ok: true } }); });
  await page.route("**/api/v1/workflow/submit", (route) => { submits++; return route.fulfill({ json: { ok: true, workflow_id: "wf-review" } }); });
  await page.goto("/");
  await page.getByRole("navigation").getByRole("button", { name: "Workflows", exact: true }).click();
  await page.getByRole("button", { name: "Create workflow", exact: true }).click();
  await page.getByRole("combobox", { name: "Workflow template", exact: true }).selectOption("sop_decision");
  const draft = JSON.parse(await page.getByTestId("workflow-composer-json").inputValue());
  expect(draft.sop.stages[1].actions[0]).toMatchObject({ kind: "decision", decision: { timeout_ms: 86400000 } });
  expect(draft.sop.stages[1].actions[0].decision.choices).toEqual([
    { id: "accept", label: "Accept inspection", outcome: "continue" }, { id: "reject", label: "Reject inspection", outcome: "fail" },
  ]);
  expect(submits).toBe(0);
  expect(choices).toBe(0);
  await page.getByRole("button", { name: "Submit workflow", exact: true }).click();
  await expect.poll(() => submits).toBe(1);
  expect(choices).toBe(0);
});
