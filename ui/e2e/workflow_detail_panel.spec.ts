import { expect, test } from "@playwright/test";

test("workflow detail panel loads summary and DAG from selected workflow", async ({ page }) => {
  let reconciliation: unknown = null;
  let deviceChecks = 0;
  let cancelled = false;
  await page.route("**/api/v1/workflow/reconcile", async (route) => {
    reconciliation = route.request().postDataJSON();
    if (route.request().postDataJSON().task_id === "task-c") {
      deviceChecks++;
      await new Promise((resolve) => setTimeout(resolve, 200));
      await route.fulfill({ status: deviceChecks === 1 ? 409 : 200,
        json: deviceChecks === 1 ? {ok:false,error:"No matching signed device receipt is recorded"} :
          {ok:true,workflow_id:"wf-detail-1",task_id:"task-c",kind:"device_receipt",node_id:"original"} });
      return;
    }
    await route.fulfill({ json: { ok: true, workflow_id: "wf-detail-1", task_id: "task-b", effect_id: "fx-fixture" } });
  });
  await page.addInitScript(() => {
    try {
      window.localStorage.setItem("agentui.simpleMode", "false");
      window.localStorage.setItem("agentui.workflowPanelOpen", JSON.stringify(true));
      window.localStorage.setItem("agentui.connectionMode", JSON.stringify("direct"));
      window.localStorage.setItem("agentui.base", "http://127.0.0.1:7777");
      window.localStorage.setItem("agentui.showSettings", "false");
    } catch {
      // ignore
    }
  });

  await page.route("**/api/v1/workflows**", async (route) => {
    await route.fulfill({
      status: 200,
      contentType: "application/json",
      body: JSON.stringify({
        ok: true,
        workflows: [
          {
            workflow_id: "wf-detail-1",
            status: "running",
            trace_id: "trace-123",
            session_id: "session-456",
            updated_unix_ms: 1700000100000,
            cancel_requested: false,
          },
        ],
      }),
    });
  });

  await page.route(/\/api\/v1\/workflow\?workflow_id=wf-detail-1.*/, async (route) => {
    await route.fulfill({
      status: 200,
      contentType: "application/json",
      body: JSON.stringify({
        ok: true,
        workflow: {
          workflow_id: "wf-detail-1",
          status: "running",
          trace_id: "trace-123",
          session_id: "session-456",
          idempotency_key: "idk-789",
          priority: 5,
          created_unix_ms: 1700000000000,
          updated_unix_ms: 1700000100000,
          deadline_unix_ms: 1700003600000,
          cancel_requested: cancelled,
        },
        tasks: [
          {
            task_id: "task-a",
            status: "error",
            blocker: {kind:"host_tool_outcome_unknown",required_action:"Inspect the original host operation before repeating it."},
            depends_on: [],
            attempt: 1,
            max_attempts: 1,
          },
          {
            task_id: "task-b",
            status: "error",
            blocker: { required_action: "Verify the original device operation and its readback." },
            depends_on: ["task-a"],
            attempt: 1,
            max_attempts: 2,
          },
          {
            task_id: "task-c", status: "error", depends_on: [],
            blocker: {kind:"device_outcome_unknown", required_action:"Obtain the original device result, then check its receipt."},
            attempt: 1, max_attempts: 1,
          },
          {
            task_id: "task-d", status: "error", depends_on: [],
            blocker: {kind:"model_run_outcome_unknown", required_action:"Inspect prior model-selected host actions; do not rerun this incomplete run."},
            attempt: 1, max_attempts: 8,
          },
          { task_id: "task-e", status: "error", depends_on: [], attempt: 2, max_attempts: 2, error: "Provider unavailable" },
        ],
        effects: [{effect_id:"mfx-saved",task_id:"task-e",request_hash:"sha256:saved",mode:"guarded",boundary:"model_run",state:"dispatching",revision:4,attempt:1,updated_unix_ms:1700000100000,resume_available:true}, {effect_id:"mfx-fixture",task_id:"task-d",request_hash:"sha256:model",mode:"guarded",boundary:"model_run",state:"unknown",revision:2,attempt:1,updated_unix_ms:1700000100000}, {effect_id:"fx-host",task_id:"task-a",request_hash:"sha256:host",mode:"guarded",state:"unknown",revision:2,attempt:1,updated_unix_ms:1700000100000}, {effect_id: "fx-fixture", task_id: "task-b", request_hash: "sha256:fixture", mode: "guarded", state: "unknown", revision: 2, attempt: 1, updated_unix_ms: 1700000100000}],
        workflow_limits: { max_steps: 12 },
        workflow_usage: { steps: 4 },
        workflow_remaining: { steps: 8 },
        result: { ok: true, message: "partial" },
        spec: {
          tasks: [
            { id: "task-a", kind: "llm" },
            { id: "task-b", kind: "llm", depends_on: ["task-a"] },
          ],
        },
      }),
    });
  });

  await page.goto("/");

  await page.getByRole("button", { name: "Workflows" }).click();
  await expect(page.getByTestId("workflow-list-panel")).toBeVisible();

  await page.getByTestId("workflow-list-panel").getByText("wf-detail-1").click();

  await expect(page.getByTestId("workflow-detail-panel")).toBeVisible();
  await expect(page.getByRole("region", { name: "Needs your attention" })).toContainText("Verify the original device operation and its readback.");
  await expect(page.getByTestId("workflow-detail-panel")).toContainText("Workflow summary");
  await expect(page.getByTestId("workflow-detail-panel")).toContainText("trace-123");
  await expect(page.getByTestId("workflow-dag-panel")).toContainText("tasks: 5");
  await expect(page.getByTestId("workflow-dag-panel")).toContainText("task-a");
  await expect(page.getByTestId("workflow-dag-panel")).toContainText("task-b");
  await expect(page.getByTestId("workflow-effects")).toContainText("Outcome unresolved");
  await expect(page.getByTestId("workflow-effects")).toContainText("Independent tasks can continue");
  await expect(page.getByTestId("workflow-effects")).toContainText("No automatic receipt resolver is available for this tool.");
  await expect(page.getByRole("textbox",{name:"Receipt endpoint for task-a"})).toHaveCount(0);
  await expect(page.getByRole("textbox",{name:"Receipt endpoint for task-d"})).toHaveCount(0);
  await expect(page.getByTestId("workflow-effects")).toContainText("Model-run host boundary");
  await expect(page.getByTestId("workflow-effects")).toContainText("Verified progress saved");
  await expect(page.getByTestId("workflow-effects")).toContainText("No manual resume control is available");
  await expect(page.getByRole("textbox",{name:"Receipt endpoint for task-e"})).toHaveCount(0);
  await expect(page.getByTestId("workflow-effects")).toContainText("Automatic rerun and transcript resume are disabled");
  await expect(page.getByText("Budgets", { exact: true })).toBeVisible();
  await expect(page.getByText("Workflow result", { exact: true })).toBeVisible();
  await expect(page.getByText("Workflow spec", { exact: true })).toBeVisible();
  await page.setViewportSize({ width: 320, height: 740 });
  await expect(page.getByRole("region", { name: "Needs your attention" })).toBeVisible();
  expect(await page.getByTestId("workflow-blockers").evaluate((element) => element.scrollWidth <= element.clientWidth)).toBe(true);
  expect(await page.getByTestId("workflow-effects").evaluate((element) => element.scrollWidth <= element.clientWidth)).toBe(true);
  await page.getByTestId("workflow-effects").screenshot({ path: "test-results/workflow-model-effects-mobile.png" });
  await page.getByRole("textbox", { name: "Receipt endpoint for task-b" }).fill("http://127.0.0.1:7777/receipts");
  await page.getByRole("button", { name: "Reconcile outcome", exact: true }).click();
  await expect.poll(() => reconciliation).toEqual({ workflow_id: "wf-detail-1", task_id: "task-b", reconcile_url: "http://127.0.0.1:7777/receipts" });
  const receipt = page.getByRole("button", {name:"Check device receipt",exact:true});
  await receipt.evaluate((button: HTMLButtonElement) => {button.click(); button.click();});
  await expect(page.getByRole("button",{name:"Checking…",exact:true})).toBeDisabled();
  await expect(page.getByRole("alert")).toContainText("No matching signed device receipt");
  expect(deviceChecks).toBe(1);
  await receipt.click();
  await expect.poll(() => deviceChecks).toBe(2);
  await expect.poll(() => reconciliation).toEqual({workflow_id:"wf-detail-1",task_id:"task-c"});
  await expect(receipt).toBeEnabled();
  cancelled = true;
  await page.getByTestId("workflow-detail-panel").getByRole("button",{name:"Reload",exact:true}).click();
  await expect(receipt).toHaveCount(0);
});
