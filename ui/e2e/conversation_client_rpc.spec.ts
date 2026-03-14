import { expect, test } from "@playwright/test";

test("conversation history replays entity_apply actions into the scene and posts client acknowledgements", async ({ page }) => {
  const clientEvents: any[] = [];
  let sceneApplyBody: any = null;
  const corsHeaders = {
    "access-control-allow-origin": "*",
    "access-control-allow-methods": "GET,POST,OPTIONS",
    "access-control-allow-headers": "authorization,content-type",
  };

  await page.addInitScript(() => {
    try {
      window.localStorage.setItem("agentui.simpleMode", "false");
      window.localStorage.setItem("agentui.connectionMode", JSON.stringify("direct"));
      window.localStorage.setItem("agentui.base", "http://127.0.0.1:7789");
      window.localStorage.setItem("agentui.showSettings", "false");
      window.localStorage.setItem("agentui.allowClientRpcs", "true");
      window.localStorage.setItem("agentui.allowClientEffects", "true");
    } catch {
      // ignore
    }
  });

  await page.route("http://127.0.0.1:7789/api/v1/**", async (route, request) => {
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
              prompt: "create a scene entity",
              assistant_text: "Scene updated.",
              ok: true,
              events: [
                {
                  type: "ui_action",
                  data: {
                    tool_call_id: "tc-entity-1",
                    action: {
                      type: "client_rpc",
                      title: "Apply scene entity",
                      rpc_id: "rpc-entity-1",
                      auto_run: true,
                      rpc: {
                        kind: "entity_apply",
                        args: {
                          ops: [
                            {
                              op: "create",
                              id: "entity-alpha",
                              entity_kind: "note",
                              title: "Entity Alpha",
                              props: { text: "Created from ui_action" },
                            },
                          ],
                        },
                      },
                    },
                  },
                },
                {
                  type: "assistant_message",
                  data: {
                    assistant_content: "Scene updated.",
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

    if (method === "POST" && path === "/api/v1/session/scene/apply") {
      sceneApplyBody = request.postDataJSON();
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        headers: corsHeaders,
        body: JSON.stringify({
          ok: true,
          session_id: "default",
          updated_unix_ms: 1,
          scene: {
            "entity-alpha": {
              id: "entity-alpha",
              kind: "note",
              title: "Entity Alpha",
              props: { text: "Created from ui_action" },
              created_ms: 1,
              updated_ms: 1,
            },
          },
        }),
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

  await expect(page.getByTestId("conversation-view")).toContainText("Scene updated.");
  await expect.poll(() => clientEvents.some((event) => event?.type === "ui_action_shown")).toBeTruthy();
  await page.getByRole("button", { name: "Run RPC" }).click();
  await expect(page.getByTestId("scene-entity-entity-alpha")).toContainText("Entity Alpha");

  await expect.poll(() => sceneApplyBody).not.toBeNull();
  expect(sceneApplyBody).toEqual({
    session_id: "default",
    ops: [{ op: "create", id: "entity-alpha", entity_kind: "note", title: "Entity Alpha", props: { text: "Created from ui_action" } }],
  });

  await expect.poll(() => clientEvents.length).toBeGreaterThanOrEqual(2);
  expect(clientEvents).toEqual(
    expect.arrayContaining([
      expect.objectContaining({
        session_id: "default",
        type: "ui_action_shown",
        data: expect.objectContaining({
          tool_call_id: "tc-entity-1",
          action_type: "client_rpc",
        }),
      }),
      expect.objectContaining({
        session_id: "default",
        type: "client_rpc_result",
        data: expect.objectContaining({
          rpc_id: "rpc-entity-1",
          request_tool_call_id: "tc-entity-1",
          rpc_kind: "entity_apply",
          ok: true,
        }),
      }),
    ]),
  );
});
