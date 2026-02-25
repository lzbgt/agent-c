import { test, expect } from "@playwright/test";

test("broker team run submits inline approvals", async ({ page }) => {
  const teamId = "team-alpha";
  const runId = "run-123";
  let runPayload: any = null;

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

  await page.route("**/v1/teams**", async (route, request) => {
    const url = new URL(request.url());
    const path = url.pathname;
    if (request.method() === "GET" && path === "/v1/teams") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, teams: [{ team_id: teamId, display_name: "Team Alpha" }] }),
      });
      return;
    }
    if (request.method() === "GET" && path === `/v1/teams/${teamId}`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, team: { team_id: teamId, owner_sub: "owner" } }),
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
          members: [{ member_id: "member-1", role: "executor" }],
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

  const teamSection = page.locator("section").filter({ has: page.getByText("Teams", { exact: true }) });
  await teamSection.getByRole("button", { name: "Refresh" }).first().click();

  const teamSelect = teamSection.getByTestId("team-select");
  await expect(teamSelect).toHaveValue(teamId);

  const runSection = page.locator("section").filter({ has: page.getByText("Team run", { exact: true }) });
  await runSection.getByPlaceholder("Summarize today’s alerts").fill("Check inline approvals");

  const inlineApprovals = runSection.getByTestId("team-inline-approvals");
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

test("broker team member inline edit submits patch", async ({ page }) => {
  const teamId = "team-alpha";
  const memberId = "member-1";
  let updatePayload: any = null;
  const members: any[] = [
    {
      member_id: memberId,
      role: "executor",
      status: "active",
      agent_id: "agent-a",
      deployment_id: "dep-a",
      weight: 1,
      capabilities: ["vision"],
      meta: { backend_label: "openrouter-main" },
    },
  ];

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

  await page.route("**/v1/teams**", async (route, request) => {
    const url = new URL(request.url());
    const path = url.pathname;
    if (request.method() === "GET" && path === "/v1/teams") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, teams: [{ team_id: teamId, display_name: "Team Alpha" }] }),
      });
      return;
    }
    if (request.method() === "GET" && path === `/v1/teams/${teamId}`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, team: { team_id: teamId, owner_sub: "owner", display_name: "Team Alpha" } }),
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
          { agent_id: "agent-a", connected: true, deployments: [{ deployment_id: "dep-a" }] },
          { agent_id: "agent-b", connected: true, deployments: [{ deployment_id: "dep-b" }] },
        ],
      }),
    });
  });

  await page.goto("/");

  const teamSection = page.locator("section").filter({ has: page.getByText("Teams", { exact: true }) });
  await teamSection.getByRole("button", { name: "Refresh" }).first().click();

  const membersSection = page.locator("section").filter({ has: page.getByText("Team members", { exact: true }) });
  await expect(membersSection.getByText(memberId)).toBeVisible();

  await membersSection.getByRole("button", { name: "Edit" }).first().click();

  const editPanel = membersSection.getByTestId("team-member-edit");
  await expect(editPanel).toBeVisible();

  await editPanel.getByTestId("team-member-edit-role").fill("reviewer");
  await editPanel.getByTestId("team-member-edit-status").selectOption("paused");
  await editPanel.getByTestId("team-member-edit-weight").fill("2");
  await editPanel.getByTestId("team-member-edit-caps").fill("vision,audio");
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
  let updatePayload: any = null;

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

  await page.route("**/v1/teams**", async (route, request) => {
    const url = new URL(request.url());
    const path = url.pathname;
    if (request.method() === "GET" && path === "/v1/teams") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, teams: [{ team_id: teamId, display_name: "Team Alpha" }] }),
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

  const teamSection = page.locator("section").filter({ has: page.getByText("Teams", { exact: true }) });
  await teamSection.getByRole("button", { name: "Refresh" }).first().click();

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
    meta: { owner_notes: "tier-1", priority: "high" },
  });
});
