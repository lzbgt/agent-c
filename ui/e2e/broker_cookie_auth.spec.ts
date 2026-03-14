import { createServer } from "node:http";
import type { AddressInfo } from "node:net";
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

test("broker cookie auth exchanges a bearer token into a cookie and scrubs the JS token", async ({ page }) => {
  const agentId = "agent1";
  let authSessionExchangeCount = 0;
  let authSessionAuthorizationHeader = "";

  const server = createServer((req, res) => {
    const origin = typeof req.headers.origin === "string" ? req.headers.origin : "http://127.0.0.1:4173";
    const corsHeaders = {
      "Access-Control-Allow-Origin": origin,
      "Access-Control-Allow-Credentials": "true",
    };
    if (req.method === "OPTIONS") {
      res.writeHead(204, {
        ...corsHeaders,
        "Access-Control-Allow-Methods": "GET, POST, DELETE, OPTIONS",
        "Access-Control-Allow-Headers":
          typeof req.headers["access-control-request-headers"] === "string"
            ? req.headers["access-control-request-headers"]
            : "Authorization, Content-Type, X-Agentd-Authorization, X-Agentd-Deployment",
      });
      res.end();
      return;
    }

    const url = new URL(req.url || "/", `http://${req.headers.host || "127.0.0.1"}`);
    const path = url.pathname;

    if (req.method === "POST" && path === "/v1/auth/session") {
      authSessionExchangeCount += 1;
      authSessionAuthorizationHeader = String(req.headers.authorization || "");
      res.writeHead(200, {
        ...corsHeaders,
        "Content-Type": "application/json",
        "Set-Cookie": "broker_auth=cookie-session; Path=/; HttpOnly; SameSite=Lax",
      });
      res.end(
        JSON.stringify({
          ok: true,
          cookie_name: "broker_auth",
          auth_kind: "oidc",
          http_only: true,
          secure: false,
          same_site: "lax",
        }),
      );
      return;
    }

    if (req.method === "GET" && path === "/v1/agents") {
      res.writeHead(200, { ...corsHeaders, "Content-Type": "application/json" });
      res.end(
        JSON.stringify({
          ok: true,
          agents: [
            {
              agent_id: agentId,
              display_name: "Agent One",
              enabled: true,
              created_unix_ms: Date.now(),
              owner_sub: "owner",
              connected: true,
              deployments: [{ deployment_id: "default", connected: true }],
            },
          ],
        }),
      );
      return;
    }

    if (req.method === "GET" && path === "/v1/caps") {
      res.writeHead(200, { ...corsHeaders, "Content-Type": "application/json" });
      res.end(
        JSON.stringify({
          ok: true,
          features: {
            auth: { cookie_enabled: true },
            client_prefs: { enabled: true },
          },
        }),
      );
      return;
    }

    if (req.method === "GET" && path === "/v1/client_prefs") {
      res.writeHead(200, { ...corsHeaders, "Content-Type": "application/json" });
      res.end(
        JSON.stringify({
          ok: true,
          found: false,
          client_id: "webui",
          client_kind: "webui",
          version: 1,
          prefs: {},
        }),
      );
      return;
    }

    if (req.method === "POST" && path === "/v1/client_prefs") {
      res.writeHead(200, { ...corsHeaders, "Content-Type": "application/json" });
      res.end(
        JSON.stringify({
          ok: true,
          found: true,
          client_id: "webui",
          client_kind: "webui",
          version: 1,
          prefs: {},
        }),
      );
      return;
    }

    res.writeHead(404, { ...corsHeaders, "Content-Type": "application/json" });
    res.end(JSON.stringify({ ok: false, error: `unhandled route: ${req.method} ${path}` }));
  });

  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
  const address = server.address() as AddressInfo | null;
  const brokerBase = `http://127.0.0.1:${address?.port || 0}`;

  try {
    await page.addInitScript((mockBase: string) => {
      const originalFetch = window.fetch.bind(window);
      const state = {
        authSessionExchangeCount: 0,
        authSessionAuthorizationHeader: "",
      };
      Object.defineProperty(window, "__brokerCookieAuthMock", {
        value: state,
        configurable: true,
      });
      window.fetch = async (input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
        const request = input instanceof Request ? input : null;
        const url = typeof input === "string" ? input : input instanceof URL ? input.toString() : request?.url || "";
        const method = String(init?.method || request?.method || "GET").toUpperCase();
        if (url === `${mockBase}/v1/auth/session` && method === "POST") {
          const headers = new Headers(init?.headers ?? request?.headers ?? undefined);
          state.authSessionExchangeCount += 1;
          state.authSessionAuthorizationHeader = headers.get("Authorization") || "";
          return new Response(
            JSON.stringify({
              ok: true,
              cookie_name: "broker_auth",
              auth_kind: "oidc",
              http_only: true,
              secure: false,
              same_site: "lax",
            }),
            {
              status: 200,
              headers: { "Content-Type": "application/json" },
            },
          );
        }
        return originalFetch(input, init);
      };
    }, brokerBase);

    await seedBrokerState(page, {
      agentId,
      brokerBase,
      brokerToken: "seed-token",
      brokerCookieAuth: true,
      showSettings: true,
    });

    await page.goto("/");
    await expect
      .poll(() =>
        page.evaluate(() => {
          // eslint-disable-next-line @typescript-eslint/no-explicit-any
          const mock = (window as any).__brokerCookieAuthMock;
          return mock?.authSessionExchangeCount ?? 0;
        }),
      )
      .toBe(1);
    await page.context().addCookies([
      {
        name: "broker_auth",
        value: "cookie-session",
        url: brokerBase,
        sameSite: "Lax",
        httpOnly: true,
      },
    ]);
    await expect(page.getByTestId("broker-cookie-session-status")).toContainText("Broker auth cookie established");
    await expect(page.getByPlaceholder("Authorization bearer token for broker (OIDC JWT)")).toHaveValue("");

    const storedSecrets = await page.evaluate(() => window.sessionStorage.getItem("agentui.connectionProfileSecrets") || "");
    expect(storedSecrets).not.toContain("seed-token");
    expect(
      await page.evaluate(() => {
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        const mock = (window as any).__brokerCookieAuthMock;
        return mock?.authSessionAuthorizationHeader || "";
      }),
    ).toBe("Bearer seed-token");
  } finally {
    await new Promise<void>((resolve, reject) => {
      server.close((err) => (err ? reject(err) : resolve()));
    });
  }
});
