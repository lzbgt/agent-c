import { test, expect, type Locator, type Page } from "@playwright/test";
import { seedBrokerState } from "./brokerTestState";

async function refreshTeams(page: Page): Promise<Locator> {
  await page.getByRole("button", { name: "Teams", exact: true }).click();
  const teamSection = page.locator("section").filter({ has: page.getByText("Teams", { exact: true }) });
  await teamSection.getByRole("button", { name: "Refresh" }).first().click();
  return teamSection;
}

async function openTeamTab(teamSection: Locator, tab: "run" | "members" | "settings"): Promise<void> {
  await teamSection.getByTestId(`team-tab-${tab}`).click();
}

test("broker team run tracks cross-deployment handoff proposal and acceptance", async ({ page }) => {
  const teamId = "team-alpha";
  const runId = "run-cross-deploy";
  const now = Date.now();
  const handoffRequests: any[] = [];
  let handoffSeq = 0;
  let handoffEvents: any[] = [];

  await seedBrokerState(page);

  const agentId = "agent1";
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
        body: JSON.stringify({ ok: true, teams: [{ team_id: teamId, display_name: "Team Alpha", created_unix_ms: now }] }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/teams/${teamId}`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          team: { team_id: teamId, display_name: "Team Alpha", owner_sub: "owner", created_unix_ms: now },
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
            { member_id: "planner-1", team_id: teamId, role: "planner", status: "active", created_unix_ms: now },
            { member_id: "executor-1", team_id: teamId, role: "executor", status: "active", created_unix_ms: now },
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
    if (method === "GET" && path === `/v1/teams/${teamId}/runs`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, team_id: teamId, runs: [] }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/teams/${teamId}/runs/${runId}`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          team_id: teamId,
          team_run_id: runId,
          status: "running",
          members: [],
          handoff_events: handoffEvents,
        }),
      });
      return;
    }
    if (method === "POST" && path === `/v1/teams/${teamId}/runs/${runId}/handoff`) {
      const body = JSON.parse(request.postData() || "{}");
      handoffRequests.push(body);
      const event = { ...(body?.event || {}) };
      if (!event.handoff_id) {
        handoffSeq += 1;
        event.handoff_id = `th_${handoffSeq}`;
      }
      if (!event.kind) event.kind = "role";
      if (!event.state) event.state = "proposed";
      event.ts_unix_ms = now + handoffEvents.length + 1;
      event.event_index = handoffEvents.length + 1;
      handoffEvents = [...handoffEvents, event];
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          team_id: teamId,
          team_run_id: runId,
          handoff_events: handoffEvents,
          handoff_event_count: handoffEvents.length,
        }),
      });
      return;
    }
    await route.fallback();
  });

  await page.goto("/");

  const teamSection = await refreshTeams(page);
  await openTeamTab(teamSection, "run");

  const runSection = page.locator("section").filter({ has: page.getByText("Team run", { exact: true }) });
  await runSection.getByPlaceholder("team_run_id").fill(runId);
  await runSection.getByRole("button", { name: "Get status" }).click();

  await expect(runSection.getByText("Handoff events", { exact: true })).toBeVisible();
  await runSection.getByTestId("team-run-handoff-kind").selectOption("cross_deployment");
  await runSection.getByTestId("team-run-handoff-from-role").fill("planner");
  await runSection.getByTestId("team-run-handoff-to-role").fill("executor");
  await runSection.getByTestId("team-run-handoff-source-deployment").fill("dep-alpha");
  await runSection.getByTestId("team-run-handoff-source-session").fill("sess-alpha");
  await runSection.getByTestId("team-run-handoff-target-deployment").fill("dep-beta");
  await runSection.getByTestId("team-run-handoff-target-session").fill("sess-beta");
  await runSection.getByTestId("team-run-handoff-reason").fill("need remote specialist");
  await runSection.getByTestId("team-run-handoff-message").fill("move this task to deployment beta");
  await runSection.getByTestId("team-run-handoff-submit").click();

  await expect.poll(() => handoffRequests.length).toBe(1);
  expect(handoffRequests[0]?.event).toMatchObject({
    kind: "cross_deployment",
    state: "proposed",
    from_role: "planner",
    to_role: "executor",
    source_deployment_id: "dep-alpha",
    source_session_id: "sess-alpha",
    target_deployment_id: "dep-beta",
    target_session_id: "sess-beta",
  });

  const proposalRow = runSection.getByTestId("team-run-handoff-row-th_1-1");
  await expect(proposalRow).toContainText("cross-deployment");
  await expect(proposalRow).toContainText("dep-alpha / sess-alpha -> dep-beta / sess-beta");
  await proposalRow.getByRole("button", { name: "Accept" }).click();

  await expect.poll(() => handoffRequests.length).toBe(2);
  expect(handoffRequests[1]?.event).toMatchObject({
    handoff_id: "th_1",
    kind: "cross_deployment",
    state: "accepted",
    from_role: "planner",
    to_role: "executor",
    source_deployment_id: "dep-alpha",
    source_session_id: "sess-alpha",
    target_deployment_id: "dep-beta",
    target_session_id: "sess-beta",
  });

  const acceptedRow = runSection.getByTestId("team-run-handoff-row-th_1-2");
  await expect(acceptedRow).toContainText("cross-deployment · accepted");
  await expect(acceptedRow).toContainText("dep-alpha / sess-alpha -> dep-beta / sess-beta");
});
