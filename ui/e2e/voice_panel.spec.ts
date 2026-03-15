import { expect, test } from "@playwright/test";

test("voice panel sends session voice controls and renders durable stats", async ({ page }) => {
  const voiceControlBodies: any[] = [];
  const corsHeaders = {
    "access-control-allow-origin": "*",
    "access-control-allow-methods": "GET,POST,OPTIONS",
    "access-control-allow-headers": "authorization,content-type",
  };

  const voiceStats = {
    ok: true,
    session_id: "default",
    max_bytes: 1048576,
    scanned_events: 5,
    client_count: 1,
    result_count: 1,
    clients: [{ id: "webui-1", kind: "browser" }],
    counts: {
      media_play: 1,
      media_pause: 0,
      media_snapshot: 0,
    },
    latest_result: {
      rpc_kind: "media_play",
      ok: true,
      selector: "#voice-audio",
    },
    latest_snapshot: {
      selector: "#voice-audio",
      current_src: "data:audio/wav;base64,AAA=",
    },
    recent_results: [
      {
        rpc_kind: "media_play",
        ok: true,
        selector: "#voice-audio",
      },
    ],
  } as Record<string, any>;

  await page.addInitScript(() => {
    try {
      window.localStorage.setItem("agentui.simpleMode", "false");
      window.localStorage.setItem("agentui.connectionMode", JSON.stringify("direct"));
      window.localStorage.setItem("agentui.base", "http://127.0.0.1:7794");
      window.localStorage.setItem("agentui.showSettings", "false");
      window.localStorage.setItem("agentui.allowClientRpcs", "true");
      window.localStorage.setItem("agentui.allowClientEffects", "true");
    } catch {
      // ignore
    }
  });

  await page.route("http://127.0.0.1:7794/api/v1/**", async (route, request) => {
    const url = new URL(request.url());
    const path = url.pathname;
    const method = request.method();

    if (method === "OPTIONS") {
      await route.fulfill({ status: 204, headers: corsHeaders, body: "" });
      return;
    }

    if (method === "GET" && path === "/api/v1/health") {
      await route.fulfill({ status: 200, contentType: "application/json", headers: corsHeaders, body: JSON.stringify({ ok: true }) });
      return;
    }

    if (method === "GET" && path === "/api/v1/config") {
      await route.fulfill({ status: 200, contentType: "application/json", headers: corsHeaders, body: JSON.stringify({ ok: true }) });
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

    if (method === "GET" && path === "/api/v1/session/client_events") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        headers: corsHeaders,
        body: JSON.stringify({ ok: true, session_id: "default", count: 0, events: [] }),
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
        body: JSON.stringify({ ok: true, ui_actions: [] }),
      });
      return;
    }

    if (method === "GET" && path === "/api/v1/db/client_events") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        headers: corsHeaders,
        body: JSON.stringify({ ok: true, client_events: [] }),
      });
      return;
    }

    if (method === "POST" && path === "/api/v1/session/voice_control") {
      const body = request.postDataJSON();
      voiceControlBodies.push(body);
      const action = String(body?.action || "");
      const kind = action === "pause" ? "media_pause" : action === "snapshot" ? "media_snapshot" : "media_play";
      const counts = voiceStats.counts as Record<string, number>;
      counts[kind] = Number(counts[kind] || 0) + 1;
      voiceStats.result_count = Number(voiceStats.result_count || 0) + 1;
      voiceStats.scanned_events = Number(voiceStats.scanned_events || 0) + 1;
      voiceStats.latest_result = {
        rpc_kind: kind,
        ok: true,
        selector: body?.selector || "#voice-audio",
        url: body?.url,
      };
      if (action === "snapshot") {
        voiceStats.latest_snapshot = {
          selector: body?.selector || "#voice-audio",
          current_src: body?.url || "data:audio/wav;base64,AAA=",
        };
      }
      voiceStats.recent_results = [
        voiceStats.latest_result,
        ...(Array.isArray(voiceStats.recent_results) ? voiceStats.recent_results : []),
      ].slice(0, 5);
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        headers: corsHeaders,
        body: JSON.stringify({
          ok: true,
          session_id: "default",
          action,
          rpc_kind: kind,
          rpc_id: `rpc-${action}-${voiceControlBodies.length}`,
          tool_call_id: `tool-${action}-${voiceControlBodies.length}`,
          pending_client_execution: true,
        }),
      });
      return;
    }

    if (method === "GET" && path === "/api/v1/session/voice_stats") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        headers: corsHeaders,
        body: JSON.stringify(voiceStats),
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

  await page.getByRole("button", { name: "Voice" }).click();
  await expect(page.getByTestId("voice-panel")).toBeVisible();
  await expect(page.getByTestId("voice-stats-section")).toContainText("play 1");
  await expect(page.getByTestId("voice-panel-clients")).toContainText("webui-1");

  await page.getByTestId("voice-panel-source-url").fill("data:audio/wav;base64,BBBB");
  await page.getByTestId("voice-panel-message").fill("Operator-triggered voice output");
  await page.getByTestId("voice-panel-play").click();

  await expect.poll(() => voiceControlBodies.length).toBe(1);
  expect(voiceControlBodies[0]).toMatchObject({
    session_id: "default",
    action: "play",
    selector: "#voice-audio",
    url: "data:audio/wav;base64,BBBB",
    title: "Voice output",
    message: "Operator-triggered voice output",
    muted: true,
    controls: true,
    autoplay: true,
    loop: false,
    volume: 1,
  });
  await expect(page.getByTestId("voice-panel-last-action")).toContainText("last action: play");
  await expect(page.getByTestId("voice-stats-section")).toContainText("play 2");

  await page.getByTestId("voice-panel-pause").click();
  await page.getByTestId("voice-panel-snapshot").click();
  await expect.poll(() => voiceControlBodies.length).toBe(3);
  expect(voiceControlBodies[1]).toMatchObject({ session_id: "default", action: "pause", selector: "#voice-audio" });
  expect(voiceControlBodies[2]).toMatchObject({ session_id: "default", action: "snapshot", selector: "#voice-audio" });

  await expect(page.getByTestId("voice-stats-section")).toContainText("pause 1");
  await expect(page.getByTestId("voice-stats-section")).toContainText("snapshot 1");
  await expect(page.getByTestId("voice-panel-latest-snapshot")).toContainText("#voice-audio");
  await expect(page.getByTestId("voice-panel-recent-results")).toContainText("media_snapshot");
});
