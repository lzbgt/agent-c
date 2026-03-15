import { expect, test } from "@playwright/test";

test("db-backed voice control ui_actions auto-run media play and pause RPCs", async ({ page }) => {
  const clientEvents: any[] = [];
  const corsHeaders = {
    "access-control-allow-origin": "*",
    "access-control-allow-methods": "GET,POST,OPTIONS",
    "access-control-allow-headers": "authorization,content-type",
  };
  const audioDataUrl = "data:audio/wav;base64,UklGRiQAAABXQVZFZm10IBAAAAABAAEAESsAACJWAAACABAAZGF0YQAAAAA=";

  await page.addInitScript(() => {
    try {
      window.localStorage.setItem("agentui.simpleMode", "false");
      window.localStorage.setItem("agentui.connectionMode", JSON.stringify("direct"));
      window.localStorage.setItem("agentui.base", "http://127.0.0.1:7792");
      window.localStorage.setItem("agentui.showSettings", "false");
      window.localStorage.setItem("agentui.allowClientRpcs", "true");
      window.localStorage.setItem("agentui.allowClientEffects", "true");
    } catch {
      // ignore
    }
    try {
      const proto = HTMLMediaElement.prototype as any;
      proto.play = function () {
        (this as any).__codexPlayCount = ((this as any).__codexPlayCount ?? 0) + 1;
        return Promise.resolve();
      };
      proto.pause = function () {
        (this as any).__codexPauseCount = ((this as any).__codexPauseCount ?? 0) + 1;
      };
    } catch {
      // ignore
    }
  });

  await page.route("http://127.0.0.1:7792/api/v1/**", async (route, request) => {
    const url = new URL(request.url());
    const path = url.pathname;
    const method = request.method();

    if (method === "OPTIONS") {
      await route.fulfill({ status: 204, headers: corsHeaders, body: "" });
      return;
    }

    if (method === "GET" && path === "/api/v1/health") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        headers: corsHeaders,
        body: JSON.stringify({ ok: true }),
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

    if (method === "GET" && path === "/api/v1/sessions") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        headers: corsHeaders,
        body: JSON.stringify({ ok: true, sessions: ["default"] }),
      });
      return;
    }

    if (method === "GET" && path === "/api/v1/session") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        headers: corsHeaders,
        body: JSON.stringify({ ok: true, session_id: "default", messages: [] }),
      });
      return;
    }

    if (method === "GET" && path === "/api/v1/session/audit") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        headers: corsHeaders,
        body: JSON.stringify({ ok: true, session_id: "default", entries: [] }),
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

    if (method === "POST" && path === "/api/v1/session/scene/apply") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        headers: corsHeaders,
        body: JSON.stringify({ ok: true, session_id: "default", updated_unix_ms: 1, scene: {} }),
      });
      return;
    }

    if (method === "GET" && path === "/api/v1/session/client_events") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        headers: corsHeaders,
        body: JSON.stringify({ ok: true, session_id: "default", count: clientEvents.length, events: clientEvents }),
      });
      return;
    }

    if (method === "GET" && path === "/api/v1/db/messages") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        headers: corsHeaders,
        body: JSON.stringify({ ok: true, messages: [] }),
      });
      return;
    }

    if (method === "GET" && path === "/api/v1/db/runs") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        headers: corsHeaders,
        body: JSON.stringify({ ok: true, runs: [] }),
      });
      return;
    }

    if (method === "GET" && path === "/api/v1/db/ui_actions") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        headers: corsHeaders,
        body: JSON.stringify({
          ok: true,
          ui_actions: [
            {
              id: 2,
              run_id: 0,
              ts_unix_ms: 2,
              tool_call_id: "voice-tool-pause-1",
              action: {
                type: "client_rpc",
                title: "Voice pause",
                rpc_id: "voice-rpc-pause-1",
                auto_run: true,
                rpc: {
                  kind: "media_pause",
                  args: { selector: "#voice-audio" },
                },
              },
            },
            {
              id: 1,
              run_id: 0,
              ts_unix_ms: 1,
              tool_call_id: "voice-tool-play-1",
              action: {
                type: "client_rpc",
                title: "Voice play",
                rpc_id: "voice-rpc-play-1",
                auto_run: true,
                rpc: {
                  kind: "media_play",
                  args: {
                    id: "voice-audio",
                    url: audioDataUrl,
                    muted: true,
                    controls: true,
                  },
                },
              },
            },
          ],
        }),
      });
      return;
    }

    if (method === "GET" && path === "/api/v1/db/client_events") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        headers: corsHeaders,
        body: JSON.stringify({ ok: true, client_events: clientEvents }),
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

    await route.fulfill({
      status: 404,
      contentType: "application/json",
      headers: corsHeaders,
      body: JSON.stringify({ ok: false, error: "not_found", path, method }),
    });
  });

  await page.goto("/");

  await expect(page.locator("audio#voice-audio")).toHaveCount(1);
  await expect.poll(() => clientEvents.filter((event) => event?.type === "client_rpc_result").length).toBeGreaterThanOrEqual(2);
  await expect
    .poll(() =>
      clientEvents
        .filter((event) => event?.type === "client_rpc_result")
        .map((event) => event?.data?.rpc_kind)
        .sort(),
    )
    .toEqual(["media_pause", "media_play"]);
  expect(clientEvents).toEqual(
    expect.arrayContaining([
      expect.objectContaining({
        session_id: "default",
        type: "client_rpc_result",
        data: expect.objectContaining({
          rpc_id: "voice-rpc-play-1",
          request_tool_call_id: "voice-tool-play-1",
          rpc_kind: "media_play",
          ok: true,
        }),
      }),
      expect.objectContaining({
        session_id: "default",
        type: "client_rpc_result",
        data: expect.objectContaining({
          rpc_id: "voice-rpc-pause-1",
          request_tool_call_id: "voice-tool-pause-1",
          rpc_kind: "media_pause",
          ok: true,
        }),
      }),
    ]),
  );
});
