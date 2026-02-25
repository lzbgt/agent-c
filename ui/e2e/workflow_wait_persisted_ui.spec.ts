import { test, expect } from "@playwright/test";

test("workflow wait resume controls show for persisted wait", async ({ page }) => {
  const base = "http://127.0.0.1:8123";
  const waitKey = `${base}::direct:pid=test-profile:tlen=0`;
  const now = Date.now();

  await page.addInitScript(({ key, ts }) => {
    try {
      window.localStorage.setItem("agentui.workflowPanelOpen", JSON.stringify(true));
      window.localStorage.setItem(
        "agentui.workflowWaitByScope",
        JSON.stringify({
          [key]: {
            workflow_id: "wf-test",
            started_unix_ms: ts - 5000,
            last_status: "running",
            updated_unix_ms: ts - 2000,
          },
        }),
      );
    } catch {
      // ignore
    }
  }, { key: waitKey, ts: now });

  await page.route("**/api/v1/workflow**", async (route) => {
    await route.fulfill({
      status: 200,
      headers: {
        "Access-Control-Allow-Origin": "*",
        "Access-Control-Allow-Headers": "authorization,content-type",
        "Access-Control-Allow-Methods": "GET,POST,OPTIONS",
      },
      contentType: "application/json",
      body: JSON.stringify({ ok: false, error: "unauthorized" }),
    });
  });

  await page.goto("/");

  await expect(page.getByText("Workflows", { exact: true })).toBeVisible();
  await expect(page.getByText(/Resume wait: wf-test/)).toBeVisible();
  await expect(page.getByRole("button", { name: "Resume" })).toBeVisible();
  await expect(page.getByText(/Resume wait: wf-test/).locator("..").getByRole("button", { name: "Clear" })).toBeVisible();
});
