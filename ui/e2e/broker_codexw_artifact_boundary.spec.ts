import { expect, test } from "@playwright/test";
import { seedBrokerState } from "./brokerTestState";

const agentId = "codexw-lab";
const brokerBase = "https://broker.example.invalid";
const proxyBase = `${brokerBase}/v1/agents/${agentId}/proxy`;
const sessionId = "sess-artifacts";
const sessionScopeKey = `profile:profile-broker-test::base:${proxyBase}`;

async function seedSessionScope(page: Parameters<typeof seedBrokerState>[0]) {
  await page.addInitScript(({ key, sid }) => {
    try {
      window.localStorage.setItem("agentui.sessionByScope", JSON.stringify({ [key]: sid }));
    } catch {
      // ignore storage failures in test bootstrap
    }
  }, { key: sessionScopeKey, sid: sessionId });
}

test("broker codexw history keeps artifact UX explicit when catalog route is unsupported", async ({ page }) => {
  await seedBrokerState(page, { agentId, brokerBase, brokerToken: "test-token", showSettings: false });
  await seedSessionScope(page);

  await page.route("**/v1/**", async (route, request) => {
    const url = new URL(request.url());
    const method = request.method();
    const path = url.pathname;

    if (method === "GET" && path === `/v1/agents/${agentId}/sessions`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, sessions: [{ session_id: sessionId, thread_id: "thread-artifacts" }] }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/sessions/${sessionId}`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, session: { session_id: sessionId, thread_id: "thread-artifacts", attachment: null } }),
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
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true, session_id: sessionId, entries: [] }) });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/session/client_events`) {
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true, session_id: sessionId, count: 0, events: [] }) });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/session/artifacts`) {
      await route.fulfill({
        status: 404,
        contentType: "application/json",
        body: JSON.stringify({ ok: false, status: 404, code: "not_found", error: "not found" }),
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

    await route.abort();
  });

  await page.goto("/");
  await page.getByRole("button", { name: "Settings" }).click();
  await page.getByTestId("session-id-input").fill(sessionId);
  await expect(page.getByText("Artifact references")).toBeVisible();
  await expect(page.getByText("Broker connector mode does not expose a stable artifact catalog here.")).toBeVisible();
  await expect(page.getByText("Use transcript, shell output, session events, and service metadata as the current result surfaces")).toBeVisible();
  await expect(page.getByText("No artifacts captured yet.")).toHaveCount(0);
});
