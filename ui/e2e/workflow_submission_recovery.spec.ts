import { expect, test, type Page } from "@playwright/test";

test("late acceptance stays with original connection and retains its recovery receipt", async ({ page }) => {
  await setup(page);
  let release!: () => void;
  const gate = new Promise<void>(resolve => { release = resolve; });
  let posts = 0;
  await page.route("**/api/v1/workflow/submit", async route => {
    posts++; await gate;
    await route.fulfill({ json: { ok: true, workflow_id: "old-connection-run" } });
  });
  await open(page);
  await page.getByRole("button", { name: "Submit workflow", exact: true }).click();
  await expect.poll(() => posts).toBe(1);
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  await page.getByTestId("settings-advanced").locator(":scope > summary").click();
  await page.getByText("Connections & access", { exact: true }).click();
  await page.getByTestId("daemon-base").fill("http://127.0.0.1:8888");
  await page.getByTestId("settings-close").click();
  release();
  await expect(page.getByLabel("Unresolved submission")).toHaveCount(0);
  await expect(page.getByText("old-connection-run", { exact: true })).toHaveCount(0);
  await expect.poll(() => page.evaluate(() => Object.keys(localStorage).filter(k => k.startsWith("agentui.pendingWorkflowSubmission.")).length)).toBe(1);
  expect(posts).toBe(1);
});

async function setup(page: Page) {
  await page.addInitScript(() => {
    localStorage.setItem("agentui.connectionMode", JSON.stringify("direct"));
    localStorage.setItem("agentui.base", "http://127.0.0.1:7777");
    localStorage.setItem("agentui.showSettings", "false");
    if (!localStorage.getItem("agentui.workflowComposerJson")) localStorage.setItem("agentui.workflowComposerJson", JSON.stringify(JSON.stringify({
      allow_sessions: true, session_id: "recovery-session", tasks: [{ task_id: "wait", kind: "delay", delay_ms: 1000 }],
    })));
  });
  await page.route("**/api/v1/**", route => route.fulfill({ json: {
    ok: true, service: "agentd", features: {}, sessions: [], tools: [], messages: [], runs: [], events: [],
    entities: [], skills: [], workflows: [], schedules: [], approvals: [], prefs: {},
  } }));
}
async function open(page: Page) {
  await page.goto("/");
  await page.getByRole("navigation").getByRole("button", { name: "Workflows", exact: true }).click();
  await page.getByRole("button", { name: "Create workflow", exact: true }).click();
}
test("lost response survives reload and reconciles via GET without another POST", async ({ page }) => {
  await setup(page);
  let posts = 0, key = "";
  await page.route("**/api/v1/workflow/submit", async route => {
    posts++; key = route.request().postDataJSON().idempotency_key;
    await route.abort("failed");
  });
  let lookups = 0;
  await page.route("**/api/v1/workflow?**", route => {
    const url = new URL(route.request().url());
    if (url.searchParams.has("idempotency_key")) {
      lookups++;
      expect(url.searchParams.get("idempotency_key")).toBe(key);
      expect(url.searchParams.get("session_id")).toBe("recovery-session");
    }
    return route.fulfill({ json: { ok: true, workflow: { workflow_id: "accepted-original", status: "done",
      idempotency_key: key, session_id: "recovery-session" }, tasks: [] } });
  });
  await open(page);
  const submit = page.getByRole("button", { name: "Submit workflow", exact: true });
  await expect(submit).toBeEnabled();
  await submit.evaluate(el => { (el as HTMLButtonElement).click(); (el as HTMLButtonElement).click(); });
  await expect(page.getByLabel("Unresolved submission")).toBeVisible();
  await expect(submit).toBeDisabled();
  expect(posts).toBe(1);
  const receipt = await page.evaluate(() => Object.keys(localStorage).filter(k => k.startsWith("agentui.pendingWorkflowSubmission.")).map(k => localStorage.getItem(k)));
  expect(receipt).toHaveLength(1);
  expect(Object.keys(JSON.parse(receipt[0]!)).sort()).toEqual(["created_unix_ms", "idempotency_key", "session_id"]);
  await open(page);
  await expect(page.getByLabel("Unresolved submission")).toBeVisible();
  expect(posts).toBe(1);
  await page.getByRole("button", { name: "Check submission receipt", exact: true }).click();
  await expect(page.getByLabel("Unresolved submission")).toHaveCount(0);
  expect(posts).toBe(1);
  expect(lookups).toBe(1);
});
test("not-found never retries; local release requires explicit confirmation", async ({ page }) => {
  await setup(page);
  let posts = 0;
  await page.route("**/api/v1/workflow/submit", async route => { posts++; await route.abort("failed"); });
  await page.route("**/api/v1/workflow?**", route => route.fulfill({ status: 404, json: { ok: false, error: "workflow not found" } }));
  await open(page);
  await page.getByRole("button", { name: "Submit workflow", exact: true }).click();
  await page.getByRole("button", { name: "Check submission receipt", exact: true }).click();
  await expect(page.getByText(/No receipt found yet/)).toBeVisible();
  await expect(page.getByLabel("Unresolved submission")).toBeVisible();
  await expect(page.getByRole("button", { name: "Submit workflow", exact: true })).toBeDisabled();
  expect(posts).toBe(1);
  page.once("dialog", dialog => dialog.dismiss());
  await page.getByRole("button", { name: "Forget local receipt…", exact: true }).click();
  await expect(page.getByLabel("Unresolved submission")).toBeVisible();
  page.once("dialog", dialog => { expect(dialog.message()).toContain("may duplicate"); return dialog.accept(); });
  await page.getByRole("button", { name: "Forget local receipt…", exact: true }).click();
  await expect(page.getByLabel("Unresolved submission")).toHaveCount(0);
  await expect(page.getByRole("button", { name: "Submit workflow", exact: true })).toBeEnabled();
  expect(posts).toBe(1);
});
test("storage failure prevents execution dispatch", async ({ page }) => {
  await setup(page);
  await page.addInitScript(() => {
    const original = Storage.prototype.setItem;
    Storage.prototype.setItem = function(key, value) {
      if (key.startsWith("agentui.pendingWorkflowSubmission.")) throw new Error("simulated quota failure");
      return original.call(this, key, value);
    };
  });
  let posts = 0;
  await page.route("**/api/v1/workflow/submit", route => { posts++; return route.fulfill({ json: { ok: true, workflow_id: "bad" } }); });
  await open(page);
  await page.getByRole("button", { name: "Submit workflow", exact: true }).click();
  await expect(page.getByText(/No request sent: submission receipt could not be persisted/)).toBeVisible();
  expect(posts).toBe(0);
});
