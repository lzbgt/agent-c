import { expect, test } from "@playwright/test";

test("scene view shows latest entity by default and can reveal and collapse history", async ({ page }) => {
  test.setTimeout(20_000);
  const scene = page.locator('[data-testid="scene"]');
  const sceneSession = page.locator('[data-testid="scene-session"]');
  const historyToggle = page.locator('[data-testid="scene-history-toggle"]');
  const collapseAll = page.locator('[data-testid="scene-collapse-all"]');
  const latestEntity = page.locator('[data-testid="scene-entity-latest"]');
  const betaEntity = page.locator('[data-testid="scene-entity-beta"]');
  const alphaEntity = page.locator('[data-testid="scene-entity-alpha"]');
  const corsHeaders = {
    "access-control-allow-origin": "*",
    "access-control-allow-methods": "GET,POST,OPTIONS",
    "access-control-allow-headers": "authorization,content-type",
  };

  await page.addInitScript(() => {
    try {
      for (const key of Object.keys(window.localStorage)) {
        if (key.startsWith("agentui.scene.")) window.localStorage.removeItem(key);
      }
      window.localStorage.setItem("agentui.simpleMode", "false");
      window.localStorage.setItem("agentui.connectionMode", JSON.stringify("direct"));
      window.localStorage.setItem("agentui.base", "http://127.0.0.1:7791");
      window.localStorage.setItem("agentui.showSettings", "false");
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

    if (method === "GET" && path === "/api/v1/caps") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        headers: corsHeaders,
        body: JSON.stringify({ ok: true, limits: { upload_max_bytes: 1048576 } }),
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

    if (method === "GET" && path === "/api/v1/session/scene") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        headers: corsHeaders,
        body: JSON.stringify({
          ok: true,
          session_id: "default",
          updated_unix_ms: 3000,
          scene: {
            alpha: {
              id: "alpha",
              kind: "note",
              title: "Alpha note",
              props: { text: "oldest" },
              created_ms: 1000,
              updated_ms: 1000,
            },
            beta: {
              id: "beta",
              kind: "canvas2d",
              title: "Beta canvas",
              props: {
                width: 240,
                height: 80,
                draw: [
                  { op: "clear", color: "#111827" },
                  { op: "text", x: 12, y: 36, text: "beta canvas", fillStyle: "#f8fafc", font: "16px sans-serif" },
                ],
              },
              created_ms: 2000,
              updated_ms: 2000,
            },
            latest: {
              id: "latest",
              kind: "note",
              title: "Latest note",
              props: { text: "newest" },
              created_ms: 3000,
              updated_ms: 3000,
            },
          },
        }),
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

    if (method === "GET" && path === "/api/v1/session/client_events") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        headers: corsHeaders,
        body: JSON.stringify({ ok: true, session_id: "default", count: 0, events: [] }),
      });
      return;
    }

    if (method === "GET" && path === "/api/v1/session/artifacts") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        headers: corsHeaders,
        body: JSON.stringify({ ok: true, session_id: "default", artifacts: [] }),
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

    if (method === "GET" && path === "/api/v1/sessions") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        headers: corsHeaders,
        body: JSON.stringify({ ok: true, sessions: ["default"] }),
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

    if (method === "GET" && path === "/api/v1/health") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        headers: corsHeaders,
        body: JSON.stringify({ ok: true }),
      });
      return;
    }

    if (method === "POST" && path === "/api/v1/session/client_event") {
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

  await expect(scene).toBeVisible();
  await expect(sceneSession).toContainText("session=default");
  await expect(page.getByText("Showing latest 1; 2 hidden.")).toBeVisible();
  await expect(latestEntity).toBeVisible();
  await expect(betaEntity).toHaveCount(0);
  await expect(alphaEntity).toHaveCount(0);

  await historyToggle.click({ force: true });
  await expect(page.getByText("Showing all 3 entities.")).toBeVisible();
  await expect(betaEntity).toBeVisible();
  await expect(alphaEntity).toBeVisible();

  await collapseAll.click({ force: true });
  await expect(latestEntity).toContainText("Collapsed");
  await expect(betaEntity).toContainText("Collapsed");

  await betaEntity.locator("button").first().click({ force: true });
  await expect(betaEntity.locator("canvas")).toBeVisible();

  await historyToggle.click({ force: true });
  await expect(page.getByText("Showing latest 1; 2 hidden.")).toBeVisible();
  await expect(betaEntity).toHaveCount(0);
  await expect(alphaEntity).toHaveCount(0);
});
