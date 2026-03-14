import { expect, test } from "@playwright/test";

test("workflow schedules panel can create, pause, resume, delete, and inspect runs", async ({ page }) => {
  const schedules = [
    {
      schedule_id: "sched-1",
      status: "active",
      cron: "0 9 * * 1-5",
      timezone: "UTC",
      created_unix_ms: 1700000000000,
      updated_unix_ms: 1700000100000,
      last_tick_unix_ms: 1700000000000,
      next_tick_unix_ms: 1700003600000,
      last_error: "",
    },
  ];
  const runsBySchedule: Record<string, any[]> = {
    "sched-1": [
      {
        schedule_id: "sched-1",
        tick_unix_ms: 1700000000000,
        workflow_id: "wf-schedule-1",
        created_unix_ms: 1700000001000,
        status: "done",
      },
    ],
  };
  const createdPayloads: any[] = [];

  await page.addInitScript(() => {
    try {
      window.localStorage.setItem("agentui.simpleMode", "false");
      window.localStorage.setItem("agentui.connectionMode", JSON.stringify("direct"));
      window.localStorage.setItem("agentui.base", "http://127.0.0.1:7777");
      window.localStorage.setItem("agentui.workflowPanelOpen", JSON.stringify(true));
      window.localStorage.setItem("agentui.showSettings", "false");
    } catch {
      // ignore
    }
  });

  await page.route("**/api/v1/workflows**", async (route) => {
    await route.fulfill({
      status: 200,
      contentType: "application/json",
      body: JSON.stringify({ ok: true, workflows: [] }),
    });
  });

  await page.route(/\/api\/v1\/workflow_schedules(?:\?|$)/, async (route, request) => {
    if (request.method() === "GET") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, status: "active", schedules }),
      });
      return;
    }
    const payload = request.postDataJSON();
    createdPayloads.push(payload);
    const next = {
      schedule_id: `sched-${schedules.length + 1}`,
      status: "active",
      cron: String(payload.cron || ""),
      timezone: String(payload.timezone || "UTC"),
      created_unix_ms: 1700000200000,
      updated_unix_ms: 1700000200000,
      last_tick_unix_ms: 0,
      next_tick_unix_ms: 1700007200000,
      last_error: "",
    };
    schedules.push(next);
    runsBySchedule[next.schedule_id] = [];
    await route.fulfill({
      status: 200,
      contentType: "application/json",
      body: JSON.stringify({
        ok: true,
        schedule_id: next.schedule_id,
        status: next.status,
        next_tick_unix_ms: next.next_tick_unix_ms,
      }),
    });
  });

  await page.route(/\/api\/v1\/workflow_schedule(?:$|\/|\?)/, async (route, request) => {
    const url = new URL(request.url());
    const path = url.pathname;
    if (request.method() === "GET" && path.endsWith("/workflow_schedule/runs")) {
      const scheduleId = url.searchParams.get("schedule_id") || "";
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          schedule_id: scheduleId,
          runs: runsBySchedule[scheduleId] || [],
        }),
      });
      return;
    }
    if (request.method() === "GET") {
      const scheduleId = url.searchParams.get("schedule_id") || "";
      const schedule = schedules.find((item) => item.schedule_id === scheduleId);
      await route.fulfill({
        status: schedule ? 200 : 404,
        contentType: "application/json",
        body: JSON.stringify(schedule ? { ok: true, schedule } : { ok: false, error: "schedule not found" }),
      });
      return;
    }
    if (request.method() === "DELETE") {
      const scheduleId = url.searchParams.get("schedule_id") || "";
      const idx = schedules.findIndex((item) => item.schedule_id === scheduleId);
      if (idx >= 0) schedules.splice(idx, 1);
      delete runsBySchedule[scheduleId];
      await route.fulfill({
        status: idx >= 0 ? 200 : 404,
        contentType: "application/json",
        body: JSON.stringify(idx >= 0 ? { ok: true, schedule_id: scheduleId } : { ok: false, error: "schedule not found" }),
      });
      return;
    }
    const payload = request.postDataJSON() as { schedule_id?: string };
    const schedule = schedules.find((item) => item.schedule_id === payload.schedule_id);
    if (!schedule) {
      await route.fulfill({
        status: 404,
        contentType: "application/json",
        body: JSON.stringify({ ok: false, error: "schedule not found" }),
      });
      return;
    }
    if (path.endsWith("/workflow_schedule/pause")) {
      schedule.status = "paused";
      schedule.updated_unix_ms += 1000;
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, schedule_id: schedule.schedule_id, status: "paused" }),
      });
      return;
    }
    if (path.endsWith("/workflow_schedule/resume")) {
      schedule.status = "active";
      schedule.updated_unix_ms += 1000;
      schedule.next_tick_unix_ms = 1700010800000;
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          schedule_id: schedule.schedule_id,
          status: "active",
          next_tick_unix_ms: schedule.next_tick_unix_ms,
        }),
      });
      return;
    }
    await route.fallback();
  });

  await page.goto("/");

  await expect(page.getByTestId("workflow-schedules-panel")).toBeVisible();
  await expect(page.getByTestId("workflow-schedule-row-sched-1")).toBeVisible();

  await page.getByTestId("workflow-schedule-row-sched-1").getByRole("button").first().click();
  await expect(page.getByTestId("workflow-schedule-runs-panel")).toContainText("wf-schedule-1");

  await page.getByTestId("workflow-schedule-cron").fill("bad cron");
  await page.getByTestId("workflow-schedule-create").click();
  await expect(page.getByText("cron validation failed")).toBeVisible();

  await page.getByRole("button", { name: "insert sample spec" }).click();
  await page.getByTestId("workflow-schedule-cron").fill("0 12 * * 1-5");
  await page.getByTestId("workflow-schedule-create").click();

  await expect(page.getByTestId("workflow-schedule-row-sched-2")).toBeVisible();
  expect(createdPayloads).toHaveLength(1);
  expect(createdPayloads[0]?.timezone).toBe("UTC");
  expect(Array.isArray(createdPayloads[0]?.spec?.tasks)).toBeTruthy();

  const sched2 = page.getByTestId("workflow-schedule-row-sched-2");
  await sched2.getByRole("button", { name: "pause", exact: true }).click();
  await expect(sched2).toContainText("paused");
  await sched2.getByRole("button", { name: "resume", exact: true }).click();
  await expect(sched2).toContainText("active");
  await sched2.getByRole("button", { name: "delete", exact: true }).click();
  await expect(page.getByTestId("workflow-schedule-row-sched-2")).toHaveCount(0);
});
