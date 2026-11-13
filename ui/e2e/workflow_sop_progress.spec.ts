import { expect, test } from "@playwright/test";

test("SOP progress uses persisted stage mappings and actual statuses without writes", async ({ page }) => {
  let phase = 0;
  let writes = 0;
  const manifest = { schema: "agentd.sop.compiled.v1", id: "inspection", version: "4", stages: [
    { id: "checks", title: "Repeated inspection", task_ids: ["a:repeat:1", "a:repeat:2", "a:repeat:3"],
      iterations: [1,2,3].map(index => ({ index, task_ids: ["a:repeat:" + index] })) },
    { id: "release", task_ids: ["release"] },
  ] };
  await page.addInitScript(() => {
    localStorage.setItem("agentui.simpleMode", "false");
    localStorage.setItem("agentui.workflowPanelOpen", "true");
    localStorage.setItem("agentui.workflowIncludeSpec", "true");
    localStorage.setItem("agentui.connectionMode", JSON.stringify("direct"));
    localStorage.setItem("agentui.base", "http://127.0.0.1:7777");
    localStorage.setItem("agentui.showSettings", "false");
  });
  await page.route("**/api/v1/**", route => {
    const url = new URL(route.request().url());
    if (url.pathname.startsWith("/api/v1/workflow") && route.request().method() !== "GET") writes++;
    const workflow = { workflow_id: "sop-progress", status: phase === 2 ? "cancelled" : "running" };
    if (url.pathname === "/api/v1/workflows") return route.fulfill({ json: { ok:true, workflows:[workflow] } });
    if (url.pathname === "/api/v1/workflow") return route.fulfill({ json: {
      ok:true, workflow, spec: url.searchParams.get("include_spec") === "true" ? { sop_compiled:manifest } : undefined, tasks: [
        { task_id:"a:repeat:1", status:"done" },
        { task_id:"a:repeat:2", status:phase === 2 ? "cancelled" : phase === 1 ? "error" : "queued",
          ...(phase === 0 ? {wait:{kind:"delay",due_unix_ms:Date.now()+60000}} : {}),
          ...(phase === 1 ? {blocker:{required_action:"Inspect original receipt",kind:"outcome_unknown"}} : {}) },
        ...(phase === 2 ? [{task_id:"a:repeat:3",status:"cancelled"},{task_id:"release",status:"cancelled"}] : []),
      ],
    } });
    return route.fulfill({json:{ok:true,service:"agentd",features:{},sessions:[],tools:[],messages:[],runs:[],events:[],entities:[],skills:[],workflows:[],schedules:[],approvals:[],prefs:{}}});
  });
  await page.goto("/");
  await page.getByRole("navigation").getByRole("button",{name:"Workflows",exact:true}).click();
  await page.getByText("sop-progress", {exact:true}).first().click();
  const card=page.getByRole("region",{name:"Process progress"});
  await expect(card).toContainText("inspection · version 4");
  await expect(card).toContainText("1/3 completed");
  await expect(card).toContainText("Repetition 2: 0/1 completed · 1 waiting (delay)");
  await expect(card).toContainText("1 not loaded");
  phase=1;
  await page.getByTestId("workflow-detail-panel").getByRole("button",{name:"Reload",exact:true}).click();
  await expect(card).toContainText("1 needs attention");
  phase=2;
  await page.getByTestId("workflow-detail-panel").getByRole("button",{name:"Reload",exact:true}).click();
  await expect(card).toContainText("2 cancelled");
  await expect(card).toContainText("1/3 completed");
  await page.setViewportSize({width:320,height:800});
  expect(await card.evaluate(el=>el.scrollWidth<=el.clientWidth)).toBe(true);
  expect(writes).toBe(0);
});
