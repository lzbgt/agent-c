import { expect, test } from "@playwright/test";

test("persisted timer waits show due time and disappear when cancelled or completed", async ({ page }) => {
  let status = "running";
  let cancelled = false;
  let taskStatus = "queued";
  await page.addInitScript(() => {
    localStorage.setItem("agentui.connectionMode", JSON.stringify("direct"));
    localStorage.setItem("agentui.base", "http://127.0.0.1:7777");
    localStorage.setItem("agentui.showSettings", "false");
  });
  await page.route("**/api/v1/**", route => route.fulfill({ json: {
    ok: true, service: "agentd", version: "test", features: {}, sessions: [], tools: [], messages: [],
    runs: [], events: [], entities: [], skills: [], workflows: [], schedules: [], approvals: [], prefs: {},
  } }));
  await page.route("**/api/v1/workflows**", route => route.fulfill({ json: {
    ok: true, workflows: [{ workflow_id: "wf-timer", status, cancel_requested: cancelled }],
  } }));
  await page.route(/\/api\/v1\/workflow\?/, route => route.fulfill({ json: {
    ok: true, workflow: { workflow_id: "wf-timer", status, cancel_requested: cancelled },
    tasks: [
      { task_id: "cooling-period", status: taskStatus, attempt: 0, max_attempts: 1,
        depends_on: [], ready_unix_ms: 1893456000000, wait: { kind: "delay", due_unix_ms: 1893456000000 } },
      { task_id: "ordinary-queued", status: "queued", attempt: 0, max_attempts: 1,
        depends_on: [], ready_unix_ms: 1893456000000 },
    ],
  } }));
  await page.goto("/");
  await page.getByRole("navigation").getByRole("button", { name: "Workflows", exact: true }).click();
  await page.getByTestId("workflow-list-panel").getByText("wf-timer", { exact: true }).click();
  const waits = page.getByTestId("workflow-timer-waits");
  await expect(waits).toBeVisible();
  await expect(waits).toContainText("cooling-period");
  await expect(waits).toContainText("due");
  await expect(waits).toContainText("survive a restart");
  await expect(waits).not.toContainText("ordinary-queued");
  await page.setViewportSize({ width: 320, height: 740 });
  expect(await waits.evaluate(el => el.scrollWidth <= el.clientWidth)).toBe(true);
  cancelled = true;
  await page.getByTestId("workflow-detail-panel").getByRole("button", { name: "Reload", exact: true }).click();
  await expect(waits).toHaveCount(0);
  cancelled = false; status = "done"; taskStatus = "done";
  await page.getByTestId("workflow-detail-panel").getByRole("button", { name: "Reload", exact: true }).click();
  await expect(waits).toHaveCount(0);
});
