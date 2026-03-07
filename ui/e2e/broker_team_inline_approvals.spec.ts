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

async function openRunDetails(runSection: Locator, title: string): Promise<void> {
  await runSection.locator("summary").filter({ hasText: title }).click();
}

test("broker team run submits inline approvals", async ({ page }) => {
  const teamId = "team-alpha";
  const runId = "run-123";
  const now = Date.now();
  let runPayload: any = null;

  await seedBrokerState(page);

  await page.route("**/v1/teams**", async (route, request) => {
    const url = new URL(request.url());
    const path = url.pathname;
    if (request.method() === "GET" && path === "/v1/teams") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, teams: [{ team_id: teamId, display_name: "Team Alpha", created_unix_ms: now }] }),
      });
      return;
    }
    if (request.method() === "GET" && path === `/v1/teams/${teamId}`) {
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
    if (request.method() === "GET" && path === `/v1/teams/${teamId}/members`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          team_id: teamId,
          members: [{ member_id: "member-1", team_id: teamId, role: "executor", status: "active", created_unix_ms: now }],
        }),
      });
      return;
    }
    if (request.method() === "GET" && path === `/v1/teams/${teamId}/quorum`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          team_id: teamId,
          rules: [
            {
              rule_id: "rule-1",
              action: "team_run",
              min_approvals: 1,
              quorum_mode: "strict",
            },
          ],
        }),
      });
      return;
    }
    if (request.method() === "POST" && path === `/v1/teams/${teamId}/runs`) {
      const body = request.postData() || "{}";
      runPayload = JSON.parse(body);
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, team_id: teamId, team_run_id: runId, status: "queued" }),
      });
      return;
    }
    if (request.method() === "GET" && path === `/v1/teams/${teamId}/runs/${runId}/approvals`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, team_id: teamId, team_run_id: runId, approvals: [] }),
      });
      return;
    }
    await route.fallback();
  });

  await page.goto("/");

  const teamSection = await refreshTeams(page);

  const teamSelect = teamSection.getByTestId("team-select");
  await expect(teamSelect).toHaveValue(teamId);
  await openTeamTab(teamSection, "run");

  const runSection = page.locator("section").filter({ has: page.getByText("Team run", { exact: true }) });
  await runSection.getByPlaceholder("Summarize today’s alerts").fill("Check inline approvals");

  const inlineApprovals = runSection.getByTestId("team-inline-approvals");
  await openRunDetails(runSection, "Inline approvals (optional)");
  await inlineApprovals.getByPlaceholder("member id").fill("member-1");
  await inlineApprovals.locator("select").selectOption("approve");
  const optionalInputs = inlineApprovals.locator("input[placeholder=\"optional\"]");
  await optionalInputs.first().fill("rule-1");
  await optionalInputs.nth(1).fill("reason text");
  await inlineApprovals.getByRole("button", { name: "Add approval" }).click();

  await expect(inlineApprovals.getByText("member-1")).toBeVisible();

  await runSection.getByRole("button", { name: "Create run" }).click();

  await expect.poll(() => runPayload).not.toBeNull();
  expect(runPayload?.team?.approvals).toEqual([
    {
      member_id: "member-1",
      decision: "approve",
      rule_id: "rule-1",
      reason: "reason text",
    },
  ]);

  await expect(runSection.getByText(`run id ${runId}`)).toBeVisible();
});

test("broker team run runtime members update submits patch", async ({ page }) => {
  const teamId = "team-alpha";
  const runId = "run-789";
  const now = Date.now();
  let updatePayload: any = null;

  await seedBrokerState(page);

  await page.route("**/v1/teams**", async (route, request) => {
    const url = new URL(request.url());
    const path = url.pathname;
    if (request.method() === "GET" && path === "/v1/teams") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, teams: [{ team_id: teamId, display_name: "Team Alpha", created_unix_ms: now }] }),
      });
      return;
    }
    if (request.method() === "GET" && path === `/v1/teams/${teamId}`) {
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
    if (request.method() === "GET" && path === `/v1/teams/${teamId}/members`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, team_id: teamId, members: [] }),
      });
      return;
    }
    if (request.method() === "GET" && path === `/v1/teams/${teamId}/quorum`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, team_id: teamId, rules: [] }),
      });
      return;
    }
    if (request.method() === "GET" && path === `/v1/teams/${teamId}/runs/${runId}`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          team_id: teamId,
          team_run_id: runId,
          status: "succeeded",
          members: [],
          runtime_members: [{ member_id: "rt-1", agent_id: "agent-a", role: "executor" }],
        }),
      });
      return;
    }
    if (request.method() === "PATCH" && path === `/v1/teams/${teamId}/runs/${runId}/runtime_members`) {
      const body = request.postData() || "{}";
      updatePayload = JSON.parse(body);
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          team_id: teamId,
          team_run_id: runId,
          status: "succeeded",
          members: [],
          runtime_members: updatePayload.runtime_members,
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

  const runtimeJson = JSON.stringify(
    [
      { member_id: "rt-1", agent_id: "agent-a", role: "executor" },
      { member_id: "rt-2", agent_id: "agent-b", role: "reviewer" },
    ],
    null,
    2,
  );
  await openRunDetails(runSection, "Runtime members (optional)");
  await runSection.getByTestId("team-run-runtime-json").fill(runtimeJson);
  await openRunDetails(runSection, "Operations & approvals");
  await runSection.getByTestId("team-run-runtime-mode").selectOption("merge");
  await runSection.getByTestId("team-run-runtime-update").click();

  await expect.poll(() => updatePayload).not.toBeNull();
  expect(updatePayload).toEqual({
    mode: "merge",
    runtime_members: [
      { member_id: "rt-1", agent_id: "agent-a", role: "executor" },
      { member_id: "rt-2", agent_id: "agent-b", role: "reviewer" },
    ],
  });
});

test("broker team run runtime member toggle submits patch", async ({ page }) => {
  const teamId = "team-alpha";
  const runId = "run-900";
  const now = Date.now();
  let updatePayload: any = null;

  await seedBrokerState(page);

  await page.route("**/v1/teams**", async (route, request) => {
    const url = new URL(request.url());
    const path = url.pathname;
    if (request.method() === "GET" && path === "/v1/teams") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, teams: [{ team_id: teamId, display_name: "Team Alpha", created_unix_ms: now }] }),
      });
      return;
    }
    if (request.method() === "GET" && path === `/v1/teams/${teamId}`) {
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
    if (request.method() === "GET" && path === `/v1/teams/${teamId}/members`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, team_id: teamId, members: [] }),
      });
      return;
    }
    if (request.method() === "GET" && path === `/v1/teams/${teamId}/quorum`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, team_id: teamId, rules: [] }),
      });
      return;
    }
    if (request.method() === "GET" && path === `/v1/teams/${teamId}/runs/${runId}`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          team_id: teamId,
          team_run_id: runId,
          status: "succeeded",
          members: [],
          runtime_members: [
            { member_id: "rt-1", agent_id: "agent-a", role: "executor", status: "active" },
            { member_id: "rt-2", agent_id: "agent-b", role: "reviewer", status: "active" },
          ],
        }),
      });
      return;
    }
    if (request.method() === "PATCH" && path === `/v1/teams/${teamId}/runs/${runId}/runtime_members`) {
      const body = request.postData() || "{}";
      updatePayload = JSON.parse(body);
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          team_id: teamId,
          team_run_id: runId,
          status: "succeeded",
          members: [],
          runtime_members: updatePayload.runtime_members,
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

  await runSection.getByTestId("team-run-runtime-toggle-rt-1").click();

  await expect.poll(() => updatePayload).not.toBeNull();
  expect(updatePayload).toEqual({
    mode: "replace",
    runtime_members: [
      { member_id: "rt-1", agent_id: "agent-a", role: "executor", status: "paused" },
      { member_id: "rt-2", agent_id: "agent-b", role: "reviewer", status: "active" },
    ],
  });
});

test("broker team member inline edit submits patch", async ({ page }) => {
  const teamId = "team-alpha";
  const memberId = "member-1";
  const now = Date.now();
  let updatePayload: any = null;
  const members: any[] = [
    {
      member_id: memberId,
      team_id: teamId,
      role: "executor",
      status: "active",
      agent_id: "agent-a",
      deployment_id: "dep-a",
      weight: 1,
      capabilities: ["vision"],
      created_unix_ms: now,
      meta: { backend_label: "openrouter-main" },
    },
  ];

  await seedBrokerState(page);

  await page.route("**/v1/teams**", async (route, request) => {
    const url = new URL(request.url());
    const path = url.pathname;
    if (request.method() === "GET" && path === "/v1/teams") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, teams: [{ team_id: teamId, display_name: "Team Alpha", created_unix_ms: now }] }),
      });
      return;
    }
    if (request.method() === "GET" && path === `/v1/teams/${teamId}`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          team: { team_id: teamId, owner_sub: "owner", display_name: "Team Alpha", created_unix_ms: now },
        }),
      });
      return;
    }
    if (request.method() === "GET" && path === `/v1/teams/${teamId}/members`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, team_id: teamId, members }),
      });
      return;
    }
    if (request.method() === "PATCH" && path === `/v1/teams/${teamId}/members/${memberId}`) {
      const body = request.postData() || "{}";
      updatePayload = JSON.parse(body);
      members[0] = { ...members[0], ...updatePayload };
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, member: members[0] }),
      });
      return;
    }
    if (request.method() === "GET" && path === `/v1/teams/${teamId}/quorum`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, team_id: teamId, rules: [] }),
      });
      return;
    }
    await route.fallback();
  });

  await page.route("**/v1/agents", async (route) => {
    await route.fulfill({
      status: 200,
      contentType: "application/json",
      body: JSON.stringify({
        ok: true,
        agents: [
          {
            agent_id: "agent-a",
            enabled: true,
            created_unix_ms: now,
            owner_sub: "owner",
            connected: true,
            deployments: [{ deployment_id: "dep-a", connected: true }],
          },
          {
            agent_id: "agent-b",
            enabled: true,
            created_unix_ms: now,
            owner_sub: "owner",
            connected: true,
            deployments: [{ deployment_id: "dep-b", connected: true }],
          },
        ],
      }),
    });
  });

  await page.goto("/");

  const teamSection = await refreshTeams(page);
  await openTeamTab(teamSection, "members");

  const membersSection = page.locator("section").filter({ has: page.getByText("Team members", { exact: true }) });
  await membersSection.locator("summary").first().click();
  await expect(membersSection.getByText(memberId)).toBeVisible();

  await membersSection.getByRole("button", { name: "Edit" }).first().click();

  const editPanel = membersSection.getByTestId("team-member-edit");
  await expect(editPanel).toBeVisible();

  await editPanel.getByTestId("team-member-edit-role").fill("reviewer");
  await editPanel.getByTestId("team-member-edit-status").selectOption("paused");
  await editPanel.getByTestId("team-member-edit-weight").fill("2");
  await editPanel.locator("summary").filter({ hasText: "Advanced overrides" }).click();
  await editPanel.getByTestId("team-member-edit-caps").fill("vision,audio");
  await expect(editPanel.locator('option[value="agent-b"]')).toHaveCount(1);
  await editPanel.getByTestId("team-member-edit-agent-pick").selectOption("agent-b");
  await editPanel.getByTestId("team-member-edit-deployment").selectOption("dep-b");
  await editPanel.getByTestId("team-member-edit-backend").fill("new-backend");
  await editPanel.getByTestId("team-member-edit-model").fill("gpt-4.1-mini");
  await editPanel.getByTestId("team-member-edit-base-url").fill("https://api.openai.com/v1");
  await editPanel.getByTestId("team-member-edit-tools").fill("basic");
  await editPanel.getByTestId("team-member-edit-timeout").fill("60000");
  await editPanel
    .getByTestId("team-member-edit-meta")
    .fill("{\"notes\":\"edited\"}");

  await editPanel.getByRole("button", { name: "Save" }).click();

  await expect.poll(() => updatePayload).not.toBeNull();
  expect(updatePayload).toEqual({
    role: "reviewer",
    status: "paused",
    capabilities: ["vision", "audio"],
    meta: {
      backend_label: "new-backend",
      notes: "edited",
      run_overrides: {
        model: "gpt-4.1-mini",
        base_url: "https://api.openai.com/v1",
        tools: "basic",
        timeout_ms: 60000,
      },
    },
    agent_id: "agent-b",
    deployment_id: "dep-b",
    weight: 2,
  });
});

test("broker team settings update submits patch", async ({ page }) => {
  const teamId = "team-alpha";
  const now = Date.now();
  let updatePayload: any = null;

  await seedBrokerState(page);

  await page.route("**/v1/teams**", async (route, request) => {
    const url = new URL(request.url());
    const path = url.pathname;
    if (request.method() === "GET" && path === "/v1/teams") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, teams: [{ team_id: teamId, display_name: "Team Alpha", created_unix_ms: now }] }),
      });
      return;
    }
    if (request.method() === "GET" && path === `/v1/teams/${teamId}`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          team: {
            team_id: teamId,
            owner_sub: "owner",
            display_name: "Team Alpha",
            created_unix_ms: now,
            tags: ["ops"],
            policy_ref: "policy:old",
            shared_memory_scope_id: "scope-old",
            meta: { note: "old" },
          },
        }),
      });
      return;
    }
    if (request.method() === "PATCH" && path === `/v1/teams/${teamId}`) {
      const body = request.postData() || "{}";
      updatePayload = JSON.parse(body);
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, team: { team_id: teamId, display_name: updatePayload.display_name } }),
      });
      return;
    }
    if (request.method() === "GET" && path === `/v1/teams/${teamId}/members`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, team_id: teamId, members: [] }),
      });
      return;
    }
    if (request.method() === "GET" && path === `/v1/teams/${teamId}/quorum`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, team_id: teamId, rules: [] }),
      });
      return;
    }
    await route.fallback();
  });

  await page.goto("/");

  const teamSection = await refreshTeams(page);
  await openTeamTab(teamSection, "settings");

  const settingsSection = page.locator("section").filter({ has: page.getByText("Team settings", { exact: true }) });
  await settingsSection.locator("summary").first().click();
  const teamSettings = page.getByTestId("team-settings");
  const teamNameInput = teamSettings.getByPlaceholder("Team display name");
  await expect(teamNameInput).toHaveValue("Team Alpha");
  await teamNameInput.fill("Team Beta");
  await teamSettings.getByPlaceholder("ops,security").fill("ops,security");
  await teamSettings.getByPlaceholder("policy:high-risk").fill("policy:new");
  await teamSettings.getByPlaceholder("scope-id").fill("scope-new");
  await teamSettings.getByPlaceholder("{\"owner_notes\":\"tier-1\",\"priority\":\"high\"}").fill(
    "{\"owner_notes\":\"tier-1\",\"priority\":\"high\"}",
  );

  await teamSettings.getByRole("button", { name: "Update team" }).click();

  await expect.poll(() => updatePayload).not.toBeNull();
  expect(updatePayload).toEqual({
    display_name: "Team Beta",
    tags: ["ops", "security"],
    policy_ref: "policy:new",
    shared_memory_scope_id: "scope-new",
    meta: { owner_notes: "tier-1", priority: "high", shared_memory_mode: "read_write" },
  });
});
