import { test, expect } from "@playwright/test";

test("approval queue loads, selects, and submits a decision", async ({ page }) => {
  const now = Date.now();
  let postedDecision: any = null;
  let approvalStatus = "pending";
  const profileId = "profile-direct-approval-test";

  await page.addInitScript(({ profileId }) => {
    try {
      window.localStorage.clear();
      window.sessionStorage.clear();
      window.localStorage.setItem("agentui.connectionProfiles", JSON.stringify([
        {
          id: profileId,
          name: "daemon:127.0.0.1:8123",
          mode: "direct",
          base: "http://127.0.0.1:8123",
          brokerBase: "https://127.0.0.1:8443",
          brokerAgentId: "agent1",
          brokerDeploymentId: "",
          brokerCookieAuth: false,
          brokerAuthToken: "",
          daemonAuthToken: "",
          runOverridesEnabled: false,
        },
      ]));
      window.localStorage.setItem("agentui.connectionProfileActive", JSON.stringify(profileId));
      window.localStorage.setItem("agentui.connectionMode", JSON.stringify("direct"));
      window.localStorage.setItem("agentui.base", JSON.stringify("http://127.0.0.1:8123"));
      window.localStorage.setItem("agentui.serverPrefsEnabled", JSON.stringify(false));
      window.localStorage.setItem("agentui.serverPrefsEnabledSet", JSON.stringify(true));
      window.localStorage.setItem("agentui.showSettings", JSON.stringify(false));
      window.localStorage.setItem("agentui.brokerPanelOpen", JSON.stringify(false));
      window.localStorage.setItem("agentui.allowClientRpcs", JSON.stringify(true));
      window.localStorage.setItem("agentui.allowClientEffects", JSON.stringify(true));
    } catch {
      // ignore
    }
  }, { profileId });

  await page.route("**/api/v1/caps", async (route) => {
    await route.fulfill({
      status: 200,
      contentType: "application/json",
      body: JSON.stringify({ ok: true, features: { client_prefs: { enabled: false } } }),
    });
  });

  await page.route("**/api/v1/approvals*", async (route, request) => {
    const url = new URL(request.url());
    if (request.method() === "GET" && url.pathname === "/api/v1/approvals") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          approvals: [
            {
              approval_id: "approval-1",
              status: approvalStatus,
              tool_name: "shell_exec",
              trace_id: "trace-1",
              run_id: 42,
              created_unix_ms: now,
            },
          ],
        }),
      });
      return;
    }
    await route.fallback();
  });

  await page.route("**/api/v1/approvals/approval-1", async (route, request) => {
    if (request.method() === "GET") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          approval: {
            approval_id: "approval-1",
            status: approvalStatus,
            tool_name: "shell_exec",
            trace_id: "trace-1",
            run_id: 42,
            created_unix_ms: now,
            required_approvals: 1,
            role_constraints: ["operator"],
            decision_reason: approvalStatus === "approved" ? "approved" : "",
          },
          decisions:
            approvalStatus === "approved"
              ? [
                  {
                    id: 1,
                    approval_id: "approval-1",
                    member_id: "member-1",
                    member_role: "operator",
                    decision: "approve",
                    decision_unix_ms: now + 1,
                    note: "looks safe",
                  },
                ]
              : [],
        }),
      });
      return;
    }
    await route.fallback();
  });

  await page.route("**/api/v1/approvals/approval-1/decisions", async (route, request) => {
    if (request.method() === "POST") {
      postedDecision = request.postDataJSON();
      approvalStatus = "approved";
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          approval_id: "approval-1",
          status: "approved",
          approved: 1,
          required_approvals: 1,
          decision: {
            id: 1,
            approval_id: "approval-1",
            member_id: "member-1",
            member_role: "operator",
            decision: "approve",
            decision_unix_ms: now + 1,
            note: "looks safe",
          },
        }),
      });
      return;
    }
    await route.fallback();
  });

  await page.goto("/");
  await page.getByRole("button", { name: "Approvals" }).click();

  const filters = page.getByTestId("approval-queue-filters");
  await filters.getByRole("button", { name: "Load approvals" }).click();

  const list = page.getByTestId("approval-queue-list");
  await expect(list.getByText("approval-1")).toBeVisible();
  await list.getByRole("button", { name: "Details" }).click();

  const detail = page.getByTestId("approval-queue-detail");
  await expect(detail.getByText("Required approvals: 1")).toBeVisible();
  await detail.getByPlaceholder("member_id").fill("member-1");
  await detail.getByPlaceholder("member_role (optional)").fill("operator");
  await detail.getByPlaceholder("note (optional)").fill("looks safe");
  await detail.getByRole("button", { name: "Approve" }).click();

  expect(postedDecision).toEqual({
    member_id: "member-1",
    member_role: "operator",
    decision: "approve",
    note: "looks safe",
  });
  await expect(detail.getByText("Decision reason: approved")).toBeVisible();
  await expect(detail.getByText("member-1")).toBeVisible();
});
