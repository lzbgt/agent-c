import { expect, test, type Page } from "@playwright/test";

test("a late library response cannot cross a daemon connection change", async ({ page }) => {
  await setup(page);
  let release: (() => void) | undefined;
  const gate = new Promise<void>((resolve) => { release = resolve; });
  let oldRequest = false;
  await page.route("**/api/v1/workflow/sops**", async (route) => {
    if (new URL(route.request().url()).port === "7777") {
      oldRequest = true;
      await gate;
      return route.fulfill({ json: { ok: true, definitions: [{ ...definition(), id: "old-private-process" }] } }).catch(() => {});
    }
    return route.fulfill({ json: { ok: true, definitions: [{ ...definition(), id: "new-connection-process" }] } });
  });
  await openComposer(page);
  await expect.poll(() => oldRequest).toBe(true);
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  await page.getByTestId("settings-advanced").locator(":scope > summary").click();
  await page.getByText("Connections & access", { exact: true }).click();
  await page.getByTestId("daemon-base").fill("http://127.0.0.1:8888");
  await page.getByTestId("settings-close").click();
  const summary = page.getByText("Saved SOP versions", { exact: true });
  if (!(await page.getByTestId("workflow-sop-library").evaluate((el) => (el as HTMLDetailsElement).open))) await summary.click();
  await expect(page.getByRole("button", { name: "Use SOP new-connection-process version 1", exact: true })).toBeVisible();
  release!();
  await expect(page.getByRole("button", { name: "Use SOP old-private-process version 1", exact: true })).toHaveCount(0);
  await expect(page.getByRole("button", { name: "Use SOP new-connection-process version 1", exact: true })).toBeVisible();
});


const definition = (version = "1", registry_id = 1) => ({
  registry_id, id: "inspection", version, title: "Inspection process",
  definition_hash: "sha256:" + version.repeat(64), created_unix_ms: 1700000000000,
});

async function setup(page: Page) {
  await page.addInitScript(() => {
    localStorage.setItem("agentui.connectionMode", JSON.stringify("direct"));
    localStorage.setItem("agentui.base", "http://127.0.0.1:7777");
    localStorage.setItem("agentui.showSettings", "false");
  });
  await page.route("**/api/v1/**", (route) => route.fulfill({ json: {
    ok: true, service: "agentd", version: "test", features: {}, sessions: [], tools: [],
    messages: [], runs: [], events: [], entities: [], skills: [], workflows: [],
    schedules: [], approvals: [], prefs: {},
  } }));
}
async function openComposer(page: Page) {
  await page.goto("/");
  await page.getByRole("navigation").getByRole("button", { name: "Workflows", exact: true }).click();
  await page.getByRole("button", { name: "Create workflow", exact: true }).click();
  await page.getByText("Saved SOP versions", { exact: true }).click();
}
test("saving a version never runs it, repeated clicks are fenced, and conflicts preserve the draft", async ({ page }) => {
  await setup(page);
  let saves = 0, submits = 0;
  let release: (() => void) | undefined;
  const gate = new Promise<void>((resolve) => { release = resolve; });
  let saved = false;
  await page.route("**/api/v1/workflow/submit", async (route) => { submits++; await route.fulfill({ json: { ok: true, workflow_id: "unexpected" } }); });
  await page.route("**/api/v1/workflow/sops**", async (route) => {
    if (route.request().method() === "POST") {
      saves++;
      expect(route.request().postDataJSON().sop.schema).toBe("agentd.sop.v1");
      if (saves > 1) return route.fulfill({ status: 409, json: { ok: false, error: "SOP version already exists with different content; save a new version" } });
      await gate;
      saved = true;
      return route.fulfill({ status: 201, json: { ok: true, created: true, definition: definition() } });
    }
    return route.fulfill({ json: { ok: true, definitions: saved ? [definition()] : [] } });
  });
  await openComposer(page);
  await page.getByRole("combobox", { name: "Workflow template", exact: true }).selectOption("sop");
  const editor = page.getByTestId("workflow-composer-json");
  const draft = await editor.inputValue();
  await page.getByRole("button", { name: "Save SOP version", exact: true }).evaluate((el) => { (el as HTMLButtonElement).click(); (el as HTMLButtonElement).click(); });
  await expect(page.getByRole("button", { name: "Saving SOP…" })).toBeDisabled();
  await expect.poll(() => saves).toBe(1);
  release!();
  await expect(page.getByText("Saved inspection · 1. Nothing has run.", { exact: true })).toBeVisible();
  expect(submits).toBe(0);
  await expect(editor).toHaveValue(draft);
  await page.getByRole("button", { name: "Save SOP version", exact: true }).click();
  await expect(page.getByTestId("workflow-sop-library").getByRole("alert")).toContainText("save a new version");
  await expect(editor).toHaveValue(draft);
  expect(submits).toBe(0);
});

test("paged versions load a hash-pinned reference and submit only on explicit run", async ({ page }) => {
  await setup(page);
  let submitted: unknown;
  const newest = definition("2", 2), older = definition();
  await page.route("**/api/v1/workflow/sops**", (route) => {
    const olderPage = new URL(route.request().url()).searchParams.has("before_id");
    return route.fulfill({ json: olderPage ? { ok: true, definitions: [older] } : { ok: true, definitions: [newest], next_before_id: 2 } });
  });
  await page.route("**/api/v1/workflow/submit", (route) => {
    submitted = route.request().postDataJSON();
    return route.fulfill({ json: { ok: true, workflow_id: "wf-saved-sop" } });
  });
  await openComposer(page);
  await expect(page.getByRole("button", { name: "Use SOP inspection version 2", exact: true })).toBeVisible();
  await page.getByRole("button", { name: "Load more SOPs", exact: true }).click();
  await expect(page.getByRole("button", { name: "Load more SOPs", exact: true })).toHaveCount(0);
  await page.getByRole("button", { name: "Use SOP inspection version 1", exact: true }).click();
  const expected = { sop_ref: { id: "inspection", version: "1", definition_hash: older.definition_hash } };
  await expect(page.getByTestId("workflow-composer-json")).toHaveValue(JSON.stringify(expected, null, 2));
  expect(submitted).toBeUndefined();
  await page.getByRole("button", { name: "Submit workflow", exact: true }).click();
  await expect.poll(() => submitted).toEqual(expected);
});

test("leaving during a save keeps the workflow draft and never starts a run", async ({ page }) => {
  await setup(page);
  let saves = 0, submits = 0;
  let release: (() => void) | undefined;
  const gate = new Promise<void>((resolve) => { release = resolve; });
  await page.route("**/api/v1/workflow/sops**", async (route) => {
    if (route.request().method() === "POST") {
      saves++;
      await gate;
      return route.fulfill({ status: 201, json: { ok: true, created: true, definition: definition() } }).catch(() => {});
    }
    return route.fulfill({ json: { ok: true, definitions: [] } });
  });
  await page.route("**/api/v1/workflow/submit", (route) => { submits++; return route.fulfill({ json: { ok: true } }); });
  await openComposer(page);
  await page.getByRole("combobox", { name: "Workflow template", exact: true }).selectOption("sop");
  const draft = await page.getByTestId("workflow-composer-json").inputValue();
  await page.getByRole("button", { name: "Save SOP version", exact: true }).click();
  await expect.poll(() => saves).toBe(1);
  await page.getByRole("button", { name: "Runs", exact: true }).click();
  release!();
  await expect(page.getByTestId("workflow-composer-json")).toBeHidden();
  await page.getByRole("button", { name: "Create workflow", exact: true }).click();
  await expect(page.getByTestId("workflow-composer-json")).toHaveValue(draft);
  expect(submits).toBe(0);
  await expect(page.getByRole("navigation").getByRole("button", { name: "Workflows", exact: true })).toHaveAttribute("aria-current", "page");
});

test("library is usable at 320px and an older daemon does not block ordinary authoring", async ({ page }) => {
  await setup(page);
  await page.setViewportSize({ width: 320, height: 740 });
  let unavailable = false;
  await page.route("**/api/v1/workflow/sops**", (route) => unavailable
    ? route.fulfill({ status: 404, json: { ok: false, error: "Saved SOP library is unavailable on this connection." } })
    : route.fulfill({ json: { ok: true, definitions: [{ ...definition(), title: "Very long inspection process name ".repeat(5) }] } }));
  await openComposer(page);
  const library = page.getByTestId("workflow-sop-library");
  await expect(page.getByRole("button", { name: "Use SOP inspection version 1", exact: true })).toBeVisible();
  expect(await library.evaluate((el) => el.scrollWidth <= el.clientWidth)).toBe(true);
  expect(await page.locator("#workspace-main").evaluate((el) => el.scrollWidth <= el.clientWidth)).toBe(true);
  await page.screenshot({ path: "test-results/saved-sop-library-320.png", fullPage: true });
  unavailable = true;
  await page.getByRole("button", { name: "Refresh SOPs", exact: true }).click();
  await expect(library.getByRole("alert")).toContainText("unavailable");
  await page.getByRole("combobox", { name: "Workflow template", exact: true }).selectOption("sop");
  await expect(page.getByTestId("workflow-composer-json")).toHaveValue(/agentd\.sop\.v1/);
  await expect(page.getByRole("button", { name: "Submit workflow", exact: true })).toBeEnabled();
});
