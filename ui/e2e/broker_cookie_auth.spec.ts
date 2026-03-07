import { test, expect } from "@playwright/test";
import { seedBrokerState } from "./brokerTestState";

test("broker cookie auth mode uses browser cookies without JS bearer headers", async ({ context, page }) => {
  const agentId = "agent1";
  const now = Date.now();
  let sawCookieRequest = false;
  let sawAuthorizationHeader = false;

  await context.addCookies([
    {
      name: "broker_auth",
      value: "cookie-session",
      domain: "broker.example.invalid",
      path: "/",
      secure: true,
      sameSite: "None",
      httpOnly: false,
    },
  ]);

  await seedBrokerState(page, {
    agentId,
    brokerToken: "",
    brokerCookieAuth: true,
  });

  await page.route("**/v1/**", async (route, request) => {
    const url = new URL(request.url());
    const method = request.method();
    const path = url.pathname;
    const headers = request.headers();

    if (url.origin === "https://broker.example.invalid") {
      if (typeof headers.cookie === "string" && headers.cookie.includes("broker_auth=cookie-session")) {
        sawCookieRequest = true;
      }
      if (typeof headers.authorization === "string" && headers.authorization.trim()) {
        sawAuthorizationHeader = true;
      }
    }

    if (method === "GET" && path === "/v1/caps") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          features: {
            auth: { cookie_enabled: true },
            client_prefs: { enabled: true },
          },
        }),
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

    await route.abort();
  });

  const agentsRequestSeen = page.waitForRequest("https://broker.example.invalid/v1/agents");
  await page.goto("/");
  await agentsRequestSeen;
  await page.getByRole("button", { name: "Agents" }).click();
  await expect(page.getByText("agent1", { exact: true })).toBeVisible();
  await expect(page.getByText("Missing broker auth token (OIDC). Set it in Settings.")).toHaveCount(0);
  expect(sawCookieRequest).toBe(true);
  expect(sawAuthorizationHeader).toBe(false);
});
