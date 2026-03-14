import { expect, test } from "@playwright/test";
import { seedBrokerState } from "./brokerTestState";

const agentId = "codexw-lab";
const brokerBase = "https://broker.example.invalid";
const proxyBase = `${brokerBase}/v1/agents/${agentId}/proxy`;
const sessionId = "sess-stream";
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

test("broker session events replay with Last-Event-ID and dedupe on reload", async ({ page }) => {
  await seedBrokerState(page, { agentId, brokerBase, brokerToken: "test-token", showSettings: true });
  await seedSessionScope(page);

  let streamRequestCount = 0;
  const replayHeaders: string[] = [];

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
          sessions: [{ session_id: sessionId, thread_id: "thread-stream" }],
        }),
      });
      return;
    }
    if (method === "POST" && path === `/v1/agents/${agentId}/sessions`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          session_id: sessionId,
          session: {
            session_id: sessionId,
            thread_id: "thread-stream",
          },
        }),
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
            thread_id: "thread-stream",
            attachment: null,
          },
        }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/sessions/${sessionId}/events`) {
      streamRequestCount += 1;
      const replayHeader = request.headers()["last-event-id"] || "";
      replayHeaders.push(replayHeader);
      if (streamRequestCount === 1) {
        await route.fulfill({
          status: 200,
          contentType: "text/event-stream",
          body: [
            `id: 41`,
            `event: agent_event`,
            `data: {"type":"assistant_message","assistant_content":"First streamed reply"}`,
            ``,
            ``,
          ].join("\n"),
        });
        return;
      }
      if (replayHeader === "41") {
        await route.fulfill({
          status: 200,
          contentType: "text/event-stream",
          body: [
            `id: 41`,
            `event: agent_event`,
            `data: {"type":"assistant_message","assistant_content":"First streamed reply"}`,
            ``,
            ``,
            `id: 42`,
            `event: agent_event`,
            `data: {"type":"assistant_message","assistant_content":"Second streamed reply"}`,
            ``,
            ``,
          ].join("\n"),
        });
        return;
      }
      await route.fulfill({
        status: 200,
        contentType: "text/event-stream",
        body: "",
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/sessions/default/events`) {
      await route.fulfill({
        status: 200,
        contentType: "text/event-stream",
        body: "",
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

    await route.abort();
  });

  await page.goto("/");
  await expect(page.getByTestId("settings-drawer")).toBeVisible();
  await page.getByTestId("session-id-input").fill(sessionId);
  await expect.poll(async () => {
    const persisted = await page.evaluate(({ key }) => {
      try {
        return JSON.parse(window.localStorage.getItem(key) || "{}");
      } catch {
        return {};
      }
    }, { key: `agentui.sessionEventStream:${sessionScopeKey}::${sessionId}` });
    return persisted?.lastEventId || "";
  }).toBe("41");
  await expect(page.getByTestId("session-stream-status")).toContainText("last_event_id: 41");
  await expect(page.getByTestId("session-stream-status")).toContainText("buffered_events: 1");

  await page.reload();

  await expect.poll(async () => {
    const persisted = await page.evaluate(({ key }) => {
      try {
        return JSON.parse(window.localStorage.getItem(key) || "{}");
      } catch {
        return {};
      }
    }, { key: `agentui.sessionEventStream:${sessionScopeKey}::${sessionId}` });
    return persisted?.lastEventId || "";
  }).toBe("42");
  await expect(page.getByTestId("session-stream-status")).toContainText("last_event_id: 42");
  await expect(page.getByTestId("session-stream-status")).toContainText("buffered_events: 2");
  const persisted = await page.evaluate(({ key }) => {
    try {
      return JSON.parse(window.localStorage.getItem(key) || "{}");
    } catch {
      return {};
    }
  }, { key: `agentui.sessionEventStream:${sessionScopeKey}::${sessionId}` });
  expect(Array.isArray(persisted?.events) ? persisted.events.map((entry: any) => entry?.id) : []).toEqual(["41", "42"]);
  expect(replayHeaders).toContain("");
  expect(replayHeaders).toContain("41");
});
