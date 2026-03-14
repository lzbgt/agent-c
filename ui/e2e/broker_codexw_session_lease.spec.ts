import { expect, test } from "@playwright/test";
import { seedBrokerState } from "./brokerTestState";

const agentId = "codexw-lab";
const brokerBase = "https://broker.example.invalid";
const proxyBase = `${brokerBase}/v1/agents/${agentId}/proxy`;
const sessionId = "sess-target";
const sessionScopeKey = `profile:profile-broker-test::base:${proxyBase}`;

async function seedSessionScope(page: Parameters<typeof seedBrokerState>[0]) {
  await page.addInitScript(({ key, sid }) => {
    try {
      window.localStorage.setItem("agentui.sessionByScope", JSON.stringify({ [key]: sid }));
      window.localStorage.setItem("agentui.sessionLeaseSeconds", JSON.stringify("120"));
    } catch {
      // ignore storage failures in test bootstrap
    }
  }, { key: sessionScopeKey, sid: sessionId });
}

test("broker codexw lease controls attach renew and release through session routes", async ({ page }) => {
  await seedBrokerState(page, { agentId, brokerBase, brokerToken: "test-token", showSettings: true });
  await seedSessionScope(page);

  let currentAttachment: null | {
    client_id: string;
    lease_seconds: number;
    lease_expires_at_ms: number | null;
    lease_active: boolean;
  } = null;

  await page.route("**/v1/**", async (route, request) => {
    const url = new URL(request.url());
    const method = request.method();
    const path = url.pathname;

    if (method === "GET" && path === `/v1/agents/${agentId}/sessions`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          sessions: [{ session_id: sessionId, thread_id: "thread-attach" }],
        }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/caps`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          features: { jobs: { enabled: true }, client_prefs: { enabled: false } },
        }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/health`) {
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true }) });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/config`) {
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true, config: {} }) });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/session/audit`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, session_id: sessionId, entries: [] }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/session/client_events`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, session_id: sessionId, count: 0, events: [] }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/session/artifacts`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, session_id: sessionId, count: 0, artifacts: [] }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/session/scene`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, session_id: sessionId, updated_unix_ms: Date.now(), scene: {} }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/db/messages`) {
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true, messages: [] }) });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/db/runs`) {
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true, runs: [] }) });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/db/ui_actions`) {
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true, rows: [] }) });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/db/client_events`) {
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true, rows: [] }) });
      return;
    }
    if (method === "GET" && path === `/v1/client_prefs`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, found: false, client_id: "webui", client_kind: "webui", version: 1, prefs: {} }),
      });
      return;
    }
    if (method === "POST" && path === `/v1/client_prefs`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, client_id: "webui", client_kind: "webui", version: 1, prefs: {} }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/session`) {
      await route.fulfill({
        status: 404,
        contentType: "application/json",
        body: JSON.stringify({ ok: false, code: "not_found", error: "not found" }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/sessions/${sessionId}`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          session: {
            session_id: sessionId,
            thread_id: "thread-attach",
            attachment: currentAttachment,
          },
        }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/session/${sessionId}`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          session: {
            session_id: sessionId,
            thread_id: "thread-attach",
            attachment: currentAttachment,
          },
        }),
      });
      return;
    }
    if (method === "POST" && path === `/v1/agents/${agentId}/sessions/${sessionId}/attach`) {
      currentAttachment = {
        client_id: "webui",
        lease_seconds: 120,
        lease_expires_at_ms: Date.now() + 120_000,
        lease_active: true,
      };
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, session: { session_id: sessionId, attachment: currentAttachment } }),
      });
      return;
    }
    if (method === "POST" && path === `/v1/agents/${agentId}/sessions/${sessionId}/attachment/renew`) {
      currentAttachment = {
        client_id: "webui",
        lease_seconds: 120,
        lease_expires_at_ms: Date.now() + 120_000,
        lease_active: true,
      };
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, attachment: currentAttachment }),
      });
      return;
    }
    if (method === "POST" && path === `/v1/agents/${agentId}/sessions/${sessionId}/attachment/release`) {
      currentAttachment = {
        client_id: "webui",
        lease_seconds: 120,
        lease_expires_at_ms: null,
        lease_active: false,
      };
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, attachment: currentAttachment }),
      });
      return;
    }

    await route.abort();
  });

  await page.goto("/");
  await expect(page.getByTestId("settings-drawer")).toBeVisible();
  await page.getByTestId("session-id-input").fill(sessionId);
  await expect(page.getByText("No active lease reported for this session.")).toBeVisible();

  await page.getByRole("button", { name: "Attach / claim" }).click();
  await expect(page.getByText("Owner: this client currently holds the attachment lease.")).toBeVisible();
  await expect(page.getByText("holder:")).toContainText("webui");

  await page.getByRole("button", { name: "Renew lease" }).click();
  await expect(page.getByText("Owner: this client currently holds the attachment lease.")).toBeVisible();

  await page.getByRole("button", { name: "Release lease" }).click();
  await expect(page.getByText("No active lease reported for this session.")).toBeVisible();
});

test("broker codexw lease conflicts surface current holder details", async ({ page }) => {
  await seedBrokerState(page, { agentId, brokerBase, brokerToken: "test-token", showSettings: true });
  await seedSessionScope(page);

  const currentAttachment = {
    client_id: "lease-owner",
    lease_seconds: 90,
    lease_expires_at_ms: 4233445566,
    lease_active: true,
  };

  await page.route("**/v1/**", async (route, request) => {
    const url = new URL(request.url());
    const method = request.method();
    const path = url.pathname;

    if (method === "GET" && path === `/v1/agents/${agentId}/sessions`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, sessions: [{ session_id: sessionId, thread_id: "thread-conflict" }] }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/caps`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, features: { jobs: { enabled: true }, client_prefs: { enabled: false } } }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/health`) {
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true }) });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/config`) {
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true, config: {} }) });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/session/audit`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, session_id: sessionId, entries: [] }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/session/client_events`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, session_id: sessionId, count: 0, events: [] }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/session/artifacts`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, session_id: sessionId, count: 0, artifacts: [] }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/session/scene`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, session_id: sessionId, updated_unix_ms: Date.now(), scene: {} }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/db/messages`) {
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true, messages: [] }) });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/db/runs`) {
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true, runs: [] }) });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/db/ui_actions`) {
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true, rows: [] }) });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/db/client_events`) {
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true, rows: [] }) });
      return;
    }
    if (method === "GET" && path === `/v1/client_prefs`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, found: false, client_id: "webui", client_kind: "webui", version: 1, prefs: {} }),
      });
      return;
    }
    if (method === "POST" && path === `/v1/client_prefs`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, client_id: "webui", client_kind: "webui", version: 1, prefs: {} }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/session`) {
      await route.fulfill({
        status: 404,
        contentType: "application/json",
        body: JSON.stringify({ ok: false, code: "not_found", error: "not found" }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/sessions/${sessionId}`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          session: {
            session_id: sessionId,
            thread_id: "thread-conflict",
            attachment: currentAttachment,
          },
        }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/session/${sessionId}`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          session: {
            session_id: sessionId,
            thread_id: "thread-conflict",
            attachment: currentAttachment,
          },
        }),
      });
      return;
    }
    if (method === "POST" && path === `/v1/agents/${agentId}/sessions/${sessionId}/attach`) {
      await route.fulfill({
        status: 409,
        contentType: "application/json",
        body: JSON.stringify({
          ok: false,
          status: 409,
          error: {
            code: "attachment_conflict",
            message: "active attachment lease blocks this mutation",
            retryable: true,
            details: {
              requested_client_id: "webui",
              current_attachment: currentAttachment,
            },
          },
        }),
      });
      return;
    }

    await route.abort();
  });

  await page.goto("/");
  await expect(page.getByTestId("settings-drawer")).toBeVisible();
  await page.getByTestId("session-id-input").fill(sessionId);
  await expect(page.getByText("Observer: lease-owner currently holds the active lease.")).toBeVisible();

  await page.getByRole("button", { name: "Attach / claim" }).click();
  await expect(page.getByText("Lease conflict")).toBeVisible();
  await expect(page.getByText("current holder:")).toContainText("lease-owner");
});
