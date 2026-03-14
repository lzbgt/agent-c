import { expect, test } from "@playwright/test";

test("memory panel queries, applies schedule, and runs retention with mocked daemon responses", async ({ page }) => {
  let scheduleUpdateBody: any = null;
  let retentionBody: any = null;

  await page.addInitScript(() => {
    try {
      window.localStorage.setItem("agentui.simpleMode", "false");
      window.localStorage.setItem("agentui.connectionMode", JSON.stringify("direct"));
      window.localStorage.setItem("agentui.base", "http://127.0.0.1:7777");
      window.localStorage.setItem("agentui.showSettings", "false");
      window.localStorage.setItem("agentui.allowClientRpcs", "true");
      window.localStorage.setItem("agentui.allowClientEffects", "true");
    } catch {
      // ignore
    }
  });

  await page.route("http://127.0.0.1:7777/api/v1/**", async (route, request) => {
    const url = new URL(request.url());
    const path = url.pathname;
    const method = request.method();

    if (method === "GET" && path === "/api/v1/memory/query") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          rows: [{ key: "ui.note", value: "captured" }],
          limit: Number(url.searchParams.get("limit") || "0"),
        }),
      });
      return;
    }

    if (method === "POST" && path === "/api/v1/memory/correlation/index") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, indexed: 14 }),
      });
      return;
    }

    if (method === "GET" && path === "/api/v1/memory/correlate") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          trace_id: url.searchParams.get("trace_id"),
          entries: [{ key: "trace.entry", value: "hit" }],
        }),
      });
      return;
    }

    if (method === "GET" && path === "/api/v1/memory/checkpoints") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, checkpoints: [{ trace_id: "trace-42" }] }),
      });
      return;
    }

    if (method === "GET" && path === "/api/v1/memory/index") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, core: [{ key: "profile" }], daily: [{ key: "daily.note" }] }),
      });
      return;
    }

    if (method === "GET" && path === "/api/v1/memory/salience") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, items: [{ key: "salient.item", score: 0.8 }] }),
      });
      return;
    }

    if (method === "GET" && path === "/api/v1/memory/recaps") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          recaps: [
            {
              recap_path: "/tmp/recaps/daily.md",
              kind: "daily",
              ts_utc: "2026-03-15T00:00:00Z",
              model: "gpt-4.1-mini",
              bytes: 321,
              summary_text: "Daily recap summary",
            },
          ],
        }),
      });
      return;
    }

    if (method === "POST" && path === "/api/v1/memory/recaps") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          generated: true,
          recaps: [{ recap_path: "/tmp/recaps/manual.md", kind: "manual" }],
        }),
      });
      return;
    }

    if (method === "GET" && path === "/api/v1/config") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          memory: {
            recap_daily_interval_ms: 60000,
            recap_weekly_interval_ms: 120000,
            recap_daily_days: 2,
            recap_weekly_days: 9,
          },
          daemon: { summary_model: "gpt-4.1" },
        }),
      });
      return;
    }

    if (method === "POST" && path === "/api/v1/config/update") {
      scheduleUpdateBody = request.postDataJSON();
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, applied: scheduleUpdateBody }),
      });
      return;
    }

    if (method === "POST" && path === "/api/v1/memory/retention/enforce") {
      retentionBody = request.postDataJSON();
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, deleted: 0, dry_run: retentionBody?.dry_run ?? true }),
      });
      return;
    }

    await route.fallback();
  });

  await page.goto("/");

  await page.getByRole("button", { name: "Memory" }).click();
  await expect(page.getByTestId("memory-panel")).toBeVisible();

  const querySection = page.getByTestId("memory-query-section");
  await querySection.getByTestId("memory-query-prefix-input").fill("ui.");
  await querySection.getByRole("button", { name: "Query" }).click();
  await expect(querySection).toContainText("ui.note");

  await querySection.getByTestId("memory-trace-id-input").fill("trace-42");
  await querySection.getByRole("button", { name: "Build index" }).click();
  await expect(querySection).toContainText("\"indexed\": 14");
  await querySection.getByRole("button", { name: "Correlate" }).click();
  await expect(querySection).toContainText("trace-42");

  const recapsSection = page.getByTestId("memory-recaps-section");
  await recapsSection.getByRole("button", { name: "Load config" }).click();
  await expect(recapsSection).toContainText("Summary model: gpt-4.1");
  await recapsSection.getByTestId("memory-recap-daily-interval-input").fill("90000");
  await recapsSection.getByRole("button", { name: "Apply schedule" }).click();
  await expect.poll(() => scheduleUpdateBody).not.toBeNull();
  expect(scheduleUpdateBody).toEqual({
    memory: {
      recap_daily_interval_ms: 90000,
      recap_weekly_interval_ms: 120000,
      recap_daily_days: 2,
      recap_weekly_days: 9,
    },
  });
  await expect(recapsSection).toContainText("recap_daily_interval_ms");

  await recapsSection.getByRole("button", { name: "List" }).click();
  await expect(recapsSection).toContainText("Daily recap summary");

  const retentionSection = page.getByTestId("memory-retention-section");
  await retentionSection.getByRole("button", { name: "Enforce" }).click();
  await expect.poll(() => retentionBody).not.toBeNull();
  expect(retentionBody).toMatchObject({
    dry_run: true,
    daily_max_days: 30,
    checkpoint_max_count: 200,
  });
  await expect(retentionSection).toContainText("\"deleted\": 0");
});
