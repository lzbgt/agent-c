import { expect, test } from "@playwright/test";
import { seedBrokerState } from "./brokerTestState";

test("broker orchestrator run panel creates loads updates and heartbeats runs", async ({ page }) => {
  const teamId = "team-alpha";
  const agentId = "agent1";
  const now = Date.now();
  const runs = [
    {
      orchestrator_run_id: "run-1",
      team_id: teamId,
      goal: "Initial goal",
      status: "running",
      created_unix_ms: now - 10_000,
      updated_unix_ms: now - 5_000,
      last_heartbeat_unix_ms: now - 4_000,
      lease_status: "ok",
      heartbeat_age_ms: 4_000,
      lease_timeout_ms: 30_000,
      meta: {
        orchestrator_owner: "planner-1",
        goal_version: 1,
        role_plan_version: 1,
        goal_versions: [],
        role_plan_versions: [],
      },
    },
  ];

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
        body: JSON.stringify({ ok: true, agent_id: agentId, owner_sub: "owner", audit: [] }),
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
      await route.fulfill({ status: 200, contentType: "text/event-stream", body: "" });
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
          members: [{ member_id: "planner-1", team_id: teamId, role: "planner", status: "active", weight: 1, created_unix_ms: now }],
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
    if (method === "GET" && path === `/v1/teams/${teamId}/orchestrator/runs`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, team_id: teamId, runs }),
      });
      return;
    }
    if (method === "POST" && path === `/v1/teams/${teamId}/orchestrator/runs`) {
      const payload = request.postDataJSON() as Record<string, any>;
      const created = {
        orchestrator_run_id: `run-${runs.length + 1}`,
        team_id: teamId,
        goal: String(payload.goal || ""),
        status: String(payload.status || "running"),
        goal_contract: payload.goal_contract,
        role_plan_snapshot: payload.role_plan_snapshot,
        meta: {
          ...(payload.meta && typeof payload.meta === "object" ? payload.meta : {}),
          goal_versions: [],
          role_plan_versions: [],
        },
        created_unix_ms: now + 1_000,
        updated_unix_ms: now + 1_000,
      };
      runs.unshift(created);
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, team_id: teamId, run: created }),
      });
      return;
    }
    if (path.startsWith(`/v1/teams/${teamId}/orchestrator/runs/`)) {
      const runId = decodeURIComponent(path.split("/").slice(-1)[0] || "");
      const targetId = path.endsWith("/heartbeat")
        ? decodeURIComponent(path.split("/").slice(-2, -1)[0] || "")
        : runId;
      const run = runs.find((item) => item.orchestrator_run_id === targetId);
      if (!run) {
        await route.fulfill({
          status: 404,
          contentType: "application/json",
          body: JSON.stringify({ ok: false, error: "run not found" }),
        });
        return;
      }
      if (method === "GET") {
        await route.fulfill({
          status: 200,
          contentType: "application/json",
          body: JSON.stringify({ ok: true, team_id: teamId, run }),
        });
        return;
      }
      if (method === "PATCH") {
        const payload = request.postDataJSON() as Record<string, any>;
        if (typeof payload.goal === "string" && payload.goal.trim()) run.goal = payload.goal.trim();
        if (typeof payload.status === "string" && payload.status.trim()) run.status = payload.status.trim();
        if ("goal_contract" in payload) run.goal_contract = payload.goal_contract;
        if ("role_plan_snapshot" in payload) run.role_plan_snapshot = payload.role_plan_snapshot;
        run.meta = {
          ...(run.meta && typeof run.meta === "object" ? run.meta : {}),
          ...(payload.meta && typeof payload.meta === "object" ? payload.meta : {}),
          orchestrator_owner: payload.expected_owner || "planner-1",
        };
        run.updated_unix_ms = now + 2_000;
        await route.fulfill({
          status: 200,
          contentType: "application/json",
          body: JSON.stringify({ ok: true, team_id: teamId, run }),
        });
        return;
      }
      if (method === "POST" && path.endsWith("/heartbeat")) {
        const payload = request.postDataJSON() as Record<string, any>;
        if (typeof payload.status === "string" && payload.status.trim()) run.status = payload.status.trim();
        run.last_heartbeat_unix_ms = now + 3_000;
        run.heartbeat_age_ms = 0;
        run.updated_unix_ms = now + 3_000;
        await route.fulfill({
          status: 200,
          contentType: "application/json",
          body: JSON.stringify({ ok: true, team_id: teamId, run }),
        });
        return;
      }
    }

    await route.fallback();
  });

  await page.goto("/");
  await page.getByRole("button", { name: "Broker Console" }).click();

  const brokerPanel = page.locator("details").filter({ has: page.getByText("Broker panel", { exact: true }) });
  await expect(brokerPanel).toBeVisible();
  await brokerPanel.getByRole("button", { name: "Teams" }).click();
  await brokerPanel.getByRole("button", { name: "Refresh" }).first().click();

  await expect(page.getByTestId("team-select")).toBeVisible();
  await expect(page.getByTestId("team-select")).toHaveValue(teamId);
  await page.getByTestId("team-tab-advanced").click();
  const orchestratorPanel = page.getByTestId("orchestrator-run-panel");
  if (!(await orchestratorPanel.isVisible())) {
    await page.locator("summary").filter({ has: page.getByText("Orchestrator runs", { exact: true }) }).first().click();
  }
  await expect(orchestratorPanel).toBeVisible();

  await expect(page.getByTestId("orchestrator-run-row-run-1")).toBeVisible();
  await page.getByTestId("orchestrator-run-create-goal").fill("Created from UI");
  await page.getByTestId("orchestrator-run-create").click();

  await expect(page.getByTestId("orchestrator-run-row-run-2")).toBeVisible();
  await expect(page.getByTestId("orchestrator-run-id")).toHaveValue("run-2");

  await page.getByTestId("orchestrator-run-row-run-1").click();
  await expect(page.getByTestId("orchestrator-run-id")).toHaveValue("run-1");
  await expect(page.getByText("Initial goal")).toBeVisible();

  await page.getByTestId("orchestrator-run-update-goal").fill("Updated goal");
  await page.getByTestId("orchestrator-run-update-status").fill("paused");
  await page.getByTestId("orchestrator-run-update").click();
  await expect(page.getByText("updated", { exact: true })).toBeVisible();
  await expect(page.getByText("Updated goal")).toBeVisible();

  await page.getByTestId("orchestrator-run-heartbeat").click();
  await expect(page.getByText("heartbeat ok", { exact: true })).toBeVisible();
});
