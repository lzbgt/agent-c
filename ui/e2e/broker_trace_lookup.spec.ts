import { test, expect } from "@playwright/test";
import { seedBrokerState } from "./brokerTestState";

test("broker trace shows agent output and failures with transport details closed", async ({ page }) => {
  await seedBrokerState(page, { brokerPanelOpen: false });
  await page.route("https://broker.example.invalid/**", async (route) => {
    const path = new URL(route.request().url()).pathname;
    if (path === "/v1/trace") return route.fulfill({ json: {
      ok: true, trace_id: "trace-broker", relay_audit: [{ agent_id: "agent1", status: 200, method: "POST", path: "/internal-transport-marker" }],
      orchestrate: [{ ts_unix_ms: 100, trace_id: "trace-broker", request_json: { internal: "orchestration-request-marker" }, response_json: { all_ok: true, tasks_total: 1 } }],
      agentd: [
        { agent_id: "agent1", ok: true, body: { ok: true, records: [{ ok: true, prompt: "Inspect the line", assistant_text: "Production checks passed." }] } },
        { agent_id: "agent2", ok: false, error: "Agent is offline." },
      ],
    } });
    return route.fulfill({ json: { ok: true, agents: [], deployments: [], members: [], teams: [], sessions: [], features: {} } });
  });
  await page.goto("/");
  await page.getByRole("navigation", { name: "Workspace navigation" }).getByRole("button", { name: "Trace", exact: true }).click();
  await page.getByLabel("Trace ID", { exact: true }).fill("trace-broker");
  await page.getByRole("button", { name: "Load", exact: true }).click();
  await expect(page.getByText("Production checks passed.", { exact: true })).toBeVisible();
  await expect(page.getByText("agent2: Agent is offline.", { exact: true })).toBeVisible();
  await expect(page.getByTestId("trace-transport-details")).not.toHaveAttribute("open", "");
  expect(await page.locator("#workspace-main").innerText()).not.toMatch(/internal-transport-marker|orchestration-request-marker/);
  await page.getByTestId("trace-transport-details").locator(":scope > summary").click();
  await page.getByText(/1 tasks/, { exact: true }).click();
  await expect(page.getByText(/orchestration-request-marker/)).toBeVisible();
});
