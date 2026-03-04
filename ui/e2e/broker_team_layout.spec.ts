import { test, expect } from "@playwright/test";

test("broker teams layout screenshot", async ({ page }, testInfo) => {
  const teamId = "team-alpha";
  const agentId = "agent1";
  const now = Date.now();

  await page.addInitScript(() => {
    try {
      window.localStorage.setItem("agentui.connectionMode", JSON.stringify("broker"));
      window.localStorage.setItem("agentui.brokerBase", "https://broker.example.invalid");
      window.localStorage.setItem("agentui.brokerAuthToken", "test-token");
      window.localStorage.setItem("agentui.brokerAgentId", "agent1");
      window.localStorage.setItem("agentui.brokerPanelOpen", "true");
      window.localStorage.setItem("agentui.showSettings", "false");
      window.localStorage.setItem("agentui.allowClientRpcs", "true");
      window.localStorage.setItem("agentui.allowClientEffects", "true");
    } catch {
      // ignore
    }
  });

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
          agents: [{ agent_id: agentId, display_name: "Agent One", connected: true }],
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
          deployments: [{ deployment_id: "default", status: "ready" }],
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
            { member_id: "planner-1", role: "planner", status: "active", weight: 1 },
            { member_id: "executor-1", role: "executor", status: "active", weight: 1 },
            { member_id: "critic-1", role: "critic", status: "active", weight: 1 },
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

  const teamSection = page.locator("section").filter({ has: page.getByText("Teams", { exact: true }) });
  await expect(teamSection).toBeVisible();
  await teamSection.getByTestId("team-tabs").getByRole("button", { name: "Setup" }).click();
  await expect(teamSection.getByText("Quick team builder")).toBeVisible();

  await page.screenshot({ path: testInfo.outputPath("broker_team_layout.png"), fullPage: true });
});
