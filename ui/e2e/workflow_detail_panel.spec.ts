import { expect, test } from "@playwright/test";

test("workflow detail panel loads summary and DAG from selected workflow", async ({ page }) => {
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
          cancel_requested: false,
        },
        tasks: [
          {
            task_id: "task-a",
            status: "done",
            depends_on: [],
            attempt: 1,
            max_attempts: 1,
          },
          {
            task_id: "task-b",
            status: "running",
            depends_on: ["task-a"],
            attempt: 1,
            max_attempts: 2,
          },
        ],
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
  await expect(page.getByTestId("workflow-detail-panel")).toContainText("Workflow summary");
  await expect(page.getByTestId("workflow-detail-panel")).toContainText("trace-123");
  await expect(page.getByTestId("workflow-dag-panel")).toContainText("tasks: 2");
  await expect(page.getByTestId("workflow-dag-panel")).toContainText("task-a");
  await expect(page.getByTestId("workflow-dag-panel")).toContainText("task-b");
  await expect(page.getByText("Budgets", { exact: true })).toBeVisible();
  await expect(page.getByText("Workflow result", { exact: true })).toBeVisible();
  await expect(page.getByText("Workflow spec", { exact: true })).toBeVisible();
});
