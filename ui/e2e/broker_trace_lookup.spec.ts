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
        { agent_id: "agent1", ok: true, body: { ok: true, records: [{
          ok: true, prompt: "Inspect the line", assistant_text: "Production checks passed.",
          events: [
            { type: "tool_result", data: { tool_name: "summary-only", status: 0, summary: { evidence: "trace-summary-marker" } } },
            { type: "tool_result", data: { tool_name: "raw-original", content: "trace-original-marker", content_truncated: false } },
            { type: "tool_result", data: { tool_name: "raw-excerpt", content: "trace-excerpt-marker", content_truncated: true } },
            { type: "tool_result", data: { tool_name: "legacy-unmarked", content: "trace-legacy-marker" } },
            { type: "tool_result", data: { tool_name: "missing-output", status: 0 } },
          ],
        }] } },
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
  const trace = page.getByTestId("trace-output");
  await expect(trace.getByText("trace-original-marker", { exact: true })).toHaveCount(0);
  await trace.getByText("Tool activity", { exact: true }).click();
  await expect(trace.getByText("trace-original-marker", { exact: true })).toBeVisible();
  await expect(trace.getByText(/trace-summary-marker/)).toBeVisible();
  await expect(trace.getByText("Unverified: this event contains a summary, not the original tool output.")).toBeVisible();
  await expect(trace.getByText("Unverified: this event contains only a truncated copy of the tool output.")).toBeVisible();
  await expect(trace.getByText("Unverified: this event does not record whether the tool output is complete.")).toBeVisible();
  await expect(trace.getByText("Unverified: this event does not include the original tool output.")).toBeVisible();
  await expect(trace.getByTestId("tool-output-copy")).toHaveCount(3);
  await expect(page.getByTestId("trace-transport-details")).not.toHaveAttribute("open", "");
  expect(await page.locator("#workspace-main").innerText()).not.toMatch(/internal-transport-marker|orchestration-request-marker/);
  await page.getByTestId("trace-transport-details").locator(":scope > summary").click();
  await page.getByText(/1 tasks/, { exact: true }).click();
  await expect(page.getByText(/orchestration-request-marker/)).toBeVisible();
});
