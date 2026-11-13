import { expect, test } from "@playwright/test";
test("guided SOP authoring preserves advanced data and submits only explicitly", async ({ page }) => {
  await page.setViewportSize({ width: 320, height: 800 });
  await page.addInitScript(() => {
    localStorage.setItem("agentui.connectionMode", JSON.stringify("direct"));
    localStorage.setItem("agentui.base", "http://127.0.0.1:7777");
    localStorage.setItem("agentui.showSettings", "false");
  });
  await page.route("**/api/v1/**", route => route.fulfill({ json: {
    ok: true, service: "agentd", features: {}, sessions: [], tools: [], messages: [],
    runs: [], events: [], entities: [], skills: [], workflows: [], schedules: [], approvals: [], prefs: {},
  } }));
  let submitted: any;
  await page.route("**/api/v1/workflow/submit", route => {
    submitted = route.request().postDataJSON();
    return route.fulfill({ json: { ok: true, workflow_id: "guided-process" } });
  });
  await page.goto("/");
  await page.getByRole("navigation").getByRole("button", { name: "Workflows", exact: true }).click();
  await page.getByRole("button", { name: "Create workflow", exact: true }).click();
  await page.getByRole("combobox", { name: "Workflow template", exact: true }).selectOption("sop");
  await expect(page.getByLabel("Process builder")).toBeVisible();
  const raw = page.getByTestId("workflow-composer-json");
  await expect(raw).toBeHidden();
  await page.getByText("Advanced process configuration", { exact: true }).click();
  const original = JSON.parse(await raw.inputValue());
  original.priority = 7;
  original.sop.stages[0].actions[0].result = { assistant_text: "Keep this exact result" };
  original.sop.stages[0].actions.push({ task_id: "absolute", kind: "timer", due_unix_ms: 1893456000000 });
  await raw.fill(JSON.stringify(original));
  await page.getByLabel("Process name", { exact: true }).fill("Factory inspection");
  await page.getByLabel("Version", { exact: true }).fill("2");
  await page.getByLabel("Action order", { exact: true }).first().selectOption("parallel");
  await page.getByLabel("Add action", { exact: true }).first().selectOption("llm");
  await page.getByLabel("Instructions", { exact: true }).fill("Summarize the inspection.");
  await page.getByLabel("Add action", { exact: true }).first().selectOption("decision");
  await page.getByLabel("Review question", { exact: true }).fill("Release this batch?");
  await page.getByRole("button", { name: "Add stage", exact: true }).click();
  await page.getByLabel("Stage name", { exact: true }).last().fill("Archive");
  await page.getByRole("button", { name: "Move stage up", exact: true }).last().click();
  const draft = JSON.parse(await raw.inputValue());
  expect(draft.priority).toBe(7);
  expect(draft.sop.stages[0].actions[0].result).toEqual(original.sop.stages[0].actions[0].result);
  expect(draft.sop.stages[0].actions[1]).toEqual(original.sop.stages[0].actions[1]);
  expect(draft.sop.stages[0].actions[2].request).toEqual({ prompt: "Summarize the inspection.", tools: "none" });
  expect(draft.sop.stages[0].actions[3].decision.prompt).toBe("Release this batch?");
  expect(draft.sop.stages[2].title).toBe("Archive");
  const ids = draft.sop.stages.flatMap((s: any) => s.actions.map((a: any) => a.task_id));
  expect(new Set(ids).size).toBe(ids.length);
  expect(submitted).toBeUndefined();
  await page.getByText("Advanced process configuration", { exact: true }).click();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
  await page.getByRole("button", { name: "Submit workflow", exact: true }).click();
  await expect.poll(() => submitted?.sop).toEqual(draft.sop);
  expect(submitted.priority).toBe(7);
});
