import { expect, test } from "@playwright/test";

test("conversation ui actions send client-state snapshots and notification acknowledgements", async ({ page }) => {
  const clientEvents: any[] = [];
  const corsHeaders = {
    "access-control-allow-origin": "*",
    "access-control-allow-methods": "GET,POST,OPTIONS",
    "access-control-allow-headers": "authorization,content-type",
  };

  await page.addInitScript(() => {
    try {
      window.localStorage.setItem("agentui.simpleMode", "false");
      window.localStorage.setItem("agentui.connectionMode", JSON.stringify("direct"));
      window.localStorage.setItem("agentui.base", "http://127.0.0.1:7790");
      window.localStorage.setItem("agentui.showSettings", "false");
      window.localStorage.setItem("agentui.allowClientRpcs", "true");
      window.localStorage.setItem("agentui.allowClientEffects", "true");
    } catch {
      // ignore
    }
  });

  await page.route("http://127.0.0.1:7790/api/v1/**", async (route, request) => {
    const url = new URL(request.url());
    const path = url.pathname;
    const method = request.method();

    if (method === "OPTIONS") {
      await route.fulfill({ status: 204, headers: corsHeaders, body: "" });
      return;
    }

    if (method === "GET" && path === "/api/v1/session/audit") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        headers: corsHeaders,
        body: JSON.stringify({
          ok: true,
          session_id: "default",
          entries: [
            {
              ts_unix_ms: 1,
              prompt: "share current browser state",
              assistant_text: "Snapshot requested.",
              ok: true,
              events: [
                {
                  type: "ui_action",
                  data: {
                    tool_call_id: "tc-state-1",
                    action: {
                      type: "request_client_state",
                      title: "Share browser state",
                      query_id: "state-query-1",
                    },
                  },
                },
                {
                  type: "ui_action",
                  data: {
                    tool_call_id: "tc-notify-1",
                    action: {
                      type: "notify",
                      title: "Operator notice",
                      message: "Review the browser state snapshot.",
                    },
                  },
                },
                {
                  type: "assistant_message",
                  data: {
                    assistant_content: "Snapshot requested.",
                  },
                },
              ],
            },
          ],
        }),
      });
      return;
    }

    if (method === "POST" && path === "/api/v1/session/client_event") {
      clientEvents.push(request.postDataJSON());
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        headers: corsHeaders,
        body: JSON.stringify({ ok: true, session_id: "default", type: "client_event" }),
      });
      return;
    }

    if (method === "GET" && path === "/api/v1/session/scene") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        headers: corsHeaders,
        body: JSON.stringify({ ok: true, session_id: "default", updated_unix_ms: 0, scene: {} }),
      });
      return;
    }

    if (method === "GET" && path === "/api/v1/db/ui_actions") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        headers: corsHeaders,
        body: JSON.stringify({ ok: true, ui_actions: [] }),
      });
      return;
    }

    if (method === "GET" && path === "/api/v1/session/client_events") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        headers: corsHeaders,
        body: JSON.stringify({ ok: true, session_id: "default", count: 0, events: [] }),
      });
      return;
    }

    if (method === "GET" && path === "/api/v1/config") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        headers: corsHeaders,
        body: JSON.stringify({ ok: true }),
      });
      return;
    }

    await route.fallback();
  });

  await page.goto("/");

  await page.getByRole("button", { name: "Show technical" }).click();
  await page.getByRole("button", { name: "Show messages" }).click();

  await expect(page.getByTestId("conversation-view")).toContainText("Snapshot requested.");
  await page.getByRole("button", { name: "Send snapshot" }).click();
  await page.getByRole("button", { name: "Acknowledge" }).click();

  await expect(page.getByRole("button", { name: "Snapshot sent" })).toBeVisible();
  await expect(page.getByRole("button", { name: "Acknowledged" })).toBeVisible();

  await expect.poll(() => clientEvents.length).toBeGreaterThanOrEqual(3);
  expect(clientEvents).toEqual(
    expect.arrayContaining([
      expect.objectContaining({
        session_id: "default",
        type: "ui_action_shown",
        data: expect.objectContaining({
          tool_call_id: "tc-state-1",
          action_type: "request_client_state",
        }),
      }),
      expect.objectContaining({
        session_id: "default",
        type: "client_state",
        data: expect.objectContaining({
          query_id: "state-query-1",
          request_tool_call_id: "tc-state-1",
          url: expect.stringContaining("/"),
          media: [],
        }),
      }),
      expect.objectContaining({
        session_id: "default",
        type: "notification_ack",
        data: expect.objectContaining({
          tool_call_id: "tc-notify-1",
          action_type: "notify",
          title: "Operator notice",
          message: "Review the browser state snapshot.",
        }),
      }),
    ]),
  );
});
