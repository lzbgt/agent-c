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
