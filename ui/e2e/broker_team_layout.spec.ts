import { test, expect } from "@playwright/test";
import fs from "fs/promises";
import path from "path";
import { seedBrokerState } from "./brokerTestState";

test("broker teams layout screenshot", async ({ page }, testInfo) => {
  const teamId = "team-alpha";
  const agentId = "agent1";
  const now = Date.now();

  await seedBrokerState(page);

  await page.route("**/v1/**", async (route, request) => {
    const url = new URL(request.url());
    const path = url.pathname;
    const method = request.method();
    if (method === "GET" && path === "/v1/agents") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          agents: [
            {
              agent_id: agentId,
              display_name: "Agent One",
              enabled: true,
              created_unix_ms: now,
              owner_sub: "owner",
              connected: true,
              deployments: [{ deployment_id: "default", connected: true }],
            },
          ],
        }),
      });
      return;
    }
    if (method === "GET" && path === "/v1/connectors") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, count: 0, connectors: [] }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/members`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          agent_id: agentId,
          owner_sub: "owner",
          members: [{ user_sub: "owner", role: "owner", created_unix_ms: now }],
        }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/membership_audit`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          agent_id: agentId,
          owner_sub: "owner",
          audit: [],
        }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/deployments`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          agent_id: agentId,
          default_deployment_id: "default",
          deployments: [{ deployment_id: "default", connected: true }],
        }),
      });
      return;
    }
    if (method === "GET" && path === "/v1/events/replay") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, events: [], count: 0, next_since_ts: now }),
      });
      return;
    }
    if (method === "GET" && path === "/v1/events") {
      await route.fulfill({
        status: 200,
        contentType: "text/event-stream",
        body: "",
      });
      return;
    }
    if (method === "GET" && path === "/v1/client_prefs") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          found: false,
          client_id: "webui",
          client_kind: "webui",
          version: 1,
          prefs: {},
        }),
      });
      return;
    }
    if (method === "POST" && path === "/v1/client_prefs") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          client_id: "webui",
          client_kind: "webui",
          version: 1,
          prefs: {},
        }),
      });
      return;
    }
    if (method === "GET" && path === "/v1/teams") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          teams: [{ team_id: teamId, display_name: "Ops Team", owner_sub: "owner", created_unix_ms: now }],
        }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/teams/${teamId}`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          team: {
            team_id: teamId,
            display_name: "Ops Team",
            owner_sub: "owner",
            created_unix_ms: now,
            tags: ["ops"],
          },
        }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/teams/${teamId}/members`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          team_id: teamId,
          members: [
            { member_id: "planner-1", team_id: teamId, role: "planner", status: "active", weight: 1, created_unix_ms: now },
            { member_id: "executor-1", team_id: teamId, role: "executor", status: "active", weight: 1, created_unix_ms: now },
            { member_id: "critic-1", team_id: teamId, role: "critic", status: "active", weight: 1, created_unix_ms: now },
          ],
        }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/teams/${teamId}/quorum`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, team_id: teamId, rules: [] }),
      });
      return;
    }
    await route.fallback();
  });

  await page.setViewportSize({ width: 1440, height: 900 });
  await page.goto("/");

  const brokerToolButton = page.getByRole("button", { name: "Broker Console" });
  await expect(brokerToolButton).toBeVisible();
  await brokerToolButton.click();

  const brokerPanel = page.locator("details").filter({ has: page.getByText("Broker panel", { exact: true }) });
  await expect(brokerPanel).toBeVisible();
  await brokerPanel.getByRole("button", { name: "Teams" }).click();

  const teamSelect = page.getByTestId("team-select");
  await expect(teamSelect).toBeVisible();

  const teamTabs = page.getByTestId("team-tabs");
  await teamTabs.getByRole("button", { name: "Setup" }).click();
  await expect(page.getByText("Quick team builder", { exact: true })).toBeVisible();

  const screenshotPath = testInfo.outputPath("broker_team_layout.png");
  await page.screenshot({ path: screenshotPath, fullPage: true });

  const outTarget = process.env.AGENT_E2E_SCREENSHOT_OUT;
  if (outTarget) {
    const targetPath = outTarget.endsWith(".png")
      ? outTarget
      : path.join(outTarget, "broker_team_layout.png");
    try {
      await fs.mkdir(path.dirname(targetPath), { recursive: true });
      await fs.copyFile(screenshotPath, targetPath);
    } catch {
      // ignore best-effort copy
    }
  }
});
