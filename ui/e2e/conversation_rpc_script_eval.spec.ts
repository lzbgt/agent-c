import { expect, test } from "@playwright/test";

test("conversation script_eval RPCs can mutate DOM through the worker bridge and post progress/results", async ({ page }) => {
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
      window.localStorage.setItem("agentui.base", "http://127.0.0.1:7791");
      window.localStorage.setItem("agentui.showSettings", "false");
      window.localStorage.setItem("agentui.allowClientRpcs", "true");
      window.localStorage.setItem("agentui.allowClientEffects", "true");
    } catch {
      // ignore
    }
  });

  await page.route("http://127.0.0.1:7791/api/v1/**", async (route, request) => {
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
              prompt: "fill an input from a worker",
              assistant_text: "Worker bridge ready.",
              ok: true,
              events: [
                {
                  type: "ui_action",
                  data: {
                    tool_call_id: "tc-script-1",
                    action: {
                      type: "client_rpc",
                      title: "Run worker script",
                      rpc_id: "rpc-script-1",
                      auto_run: false,
                      rpc: {
                        kind: "script_eval",
                        args: {
                          code: `
await api.dom.apply({
  ops: [
    { op: "create", tag: "input", attrs: { id: "rpc-script-input" } }
  ]
});
await api.dom.setValue({ selector: "#rpc-script-input", value: "from script eval" });
await api.progress("filled", { value: "from script eval" });
return await api.dom.query({ selector: "#rpc-script-input", fields: ["value"] });
                          `,
                        },
                      },
                    },
                  },
                },
                {
                  type: "assistant_message",
                  data: {
                    assistant_content: "Worker bridge ready.",
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

  await expect(page.getByTestId("conversation-view")).toContainText("Worker bridge ready.");
  await page.getByRole("button", { name: "Run RPC" }).click();

  await expect(page.locator("#rpc-script-input")).toHaveValue("from script eval");
  await expect
    .poll(() =>
      clientEvents.some(
        (event) => event?.type === "client_rpc_result" && event?.data?.rpc_id === "rpc-script-1" && event?.data?.ok === true,
      ),
    )
    .toBeTruthy();
  expect(clientEvents).toEqual(
    expect.arrayContaining([
      expect.objectContaining({
        session_id: "default",
        type: "client_rpc_progress",
        data: expect.objectContaining({
          rpc_id: "rpc-script-1",
          rpc_kind: "script_eval",
          name: "filled",
        }),
      }),
      expect.objectContaining({
        session_id: "default",
        type: "client_rpc_result",
        data: expect.objectContaining({
          rpc_id: "rpc-script-1",
          request_tool_call_id: "tc-script-1",
          rpc_kind: "script_eval",
          ok: true,
        }),
      }),
    ]),
  );
});
