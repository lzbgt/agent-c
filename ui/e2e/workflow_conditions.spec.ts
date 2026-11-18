import { expect, test } from "@playwright/test";
import { execFileSync } from "node:child_process";

test("guided conditions preserve drafts and execute exact pinned native source only on submission", async ({ page }) => {
  let writes = 0;
  let submitted: any;
  await page.addInitScript(() => {
    localStorage.setItem("agentui.connectionMode", JSON.stringify("direct"));
    localStorage.setItem("agentui.base", "http://127.0.0.1:7777");
    localStorage.setItem("agentui.showSettings", "false");
  });
  await page.route("**/api/v1/**", route => {
    if (route.request().method() !== "GET" && new URL(route.request().url()).pathname.startsWith("/api/v1/workflow")) writes++;
    return route.fulfill({ json: { ok:true, features:{}, sessions:[], tools:[], messages:[], runs:[], events:[], workflows:[], schedules:[], prefs:{} } });
  });
  await page.route("**/api/v1/workflow/submit", route => {
    writes++; submitted = route.request().postDataJSON();
    return route.fulfill({ json: { ok:true, workflow_id:"authored-condition" } });
  });
  await page.goto("/");
  await page.getByRole("navigation").getByRole("button", { name:"Workflows", exact:true }).click();
  await page.getByRole("button", { name:"Create workflow", exact:true }).click();
  await page.getByRole("combobox", { name:"Workflow template", exact:true }).selectOption("sop");
  await page.getByText("Advanced process configuration", { exact:true }).click();
  const raw = page.getByTestId("workflow-composer-json");
  const initial = { sop: { schema:"agentd.sop.v1", id:"conditional-browser", version:"1", stages:[
    { id:"prepare", actions:[{task_id:"source",kind:"delay",result:{payload:{accepted:true}}}] },
    { id:"optional-stage", actions:[{task_id:"optional",kind:"host_tool",max_attempts:3,
      host_tool:{name:"proc_exec",timeout_ms:3000,arguments:{argv:["/usr/bin/printf","not executed"]}}}] },
    { id:"finish", actions:[{task_id:"after",kind:"delay",delay_ms:0}] },
  ] } };
  await raw.fill(JSON.stringify(initial));
  const control = page.getByTestId("process-condition-optional");
  await control.getByRole("checkbox").check();
  await control.getByRole("combobox", { name:"Condition source", exact:true }).selectOption("source");
  await control.getByLabel("Result field (JSON pointer)", { exact:true }).fill("/payload/accepted");
  await control.getByRole("combobox", { name:"Expected value type", exact:true }).selectOption("text");
  await control.getByLabel("Expected text", { exact:true }).fill("literal ${task.not_a_template}");
  expect(JSON.parse(await raw.inputValue()).sop.stages[1].actions[0].when.equals).toBe("literal ${task.not_a_template}");
  await control.getByRole("combobox", { name:"Expected value type", exact:true }).selectOption("boolean");
  await control.getByRole("combobox", { name:"Expected boolean", exact:true }).selectOption("false");
  await control.getByRole("checkbox").uncheck();
  expect(JSON.parse(await raw.inputValue()).sop.stages[1].actions[0]).not.toHaveProperty("when");
  await control.getByRole("checkbox").check();
  await control.getByRole("combobox", { name:"Expected boolean", exact:true }).selectOption("false");
  await page.getByRole("group", { name:"Stage 2", exact:true }).getByRole("button", { name:"Move stage up", exact:true }).click();
  await expect(control.getByRole("alert")).toContainText("Choose an earlier action");
  expect(JSON.parse(await raw.inputValue()).sop.stages[0].actions[0].when.task_id).toBe("source");
  await page.getByRole("group", { name:"Stage 1", exact:true }).getByRole("button", { name:"Move stage down", exact:true }).click();
  await expect(control.getByRole("alert")).toHaveCount(0);
  const action = JSON.parse(await raw.inputValue()).sop.stages[1].actions[0];
  expect(action.max_attempts).toBe(3);
  expect(action.host_tool.arguments.argv).toEqual(["/usr/bin/printf","not executed"]);
  expect(action.when).toEqual({task_id:"source",pointer:"/payload/accepted",equals:false});
  await page.setViewportSize({width:320,height:800});
  expect(await control.evaluate(e => e.scrollWidth <= e.clientWidth)).toBe(true);
  expect(writes).toBe(0);
  await page.getByRole("button", { name:"Submit workflow", exact:true }).click();
  await expect.poll(() => !!submitted).toBe(true);
  expect(writes).toBe(1);
  const output = execFileSync("python3", ["../tests/workflow_condition_smoke.py",
    process.env.AGENT_CONDITION_TEST_BINARY || "../build-snapshot/agentd", "--authored"], {
      input:JSON.stringify(submitted),encoding:"utf8",timeout:45000,
    });
  expect(output).toContain("PASS exact browser-authored conditional SOP");
});

test("conditional progress distinguishes skipped work and keeps evidence details collapsed", async ({ page }) => {
  let writes = 0;
  let invalid = false;
  await page.addInitScript(() => {
    localStorage.setItem("agentui.simpleMode","false");
    localStorage.setItem("agentui.workflowPanelOpen","true");
    localStorage.setItem("agentui.workflowIncludeSpec","true");
    localStorage.setItem("agentui.connectionMode",JSON.stringify("direct"));
    localStorage.setItem("agentui.base","http://127.0.0.1:7777");
    localStorage.setItem("agentui.showSettings","false");
  });
  await page.route("**/api/v1/**", route => {
    const path = new URL(route.request().url()).pathname;
    if (path.startsWith("/api/v1/workflow") && route.request().method() !== "GET") writes++;
    const workflow = {workflow_id:"conditional-progress",status:"done"};
    if (path === "/api/v1/workflows") return route.fulfill({json:{ok:true,workflows:[workflow]}});
    if (path === "/api/v1/workflow") return route.fulfill({json:{ok:true,workflow,
      spec:{sop_compiled:{schema:"agentd.sop.compiled.v1",id:"optional-check",version:"1",
        stages:[{id:"review",task_ids:["source","optional"]}]}},
      tasks:[{task_id:"source",status:"done"},{task_id:"optional",status:invalid?"queued":"done",depends_on:["source"],
        skipped:true,condition:{task_id:"source",pointer:"/payload/accepted",matched:false}}],
    }});
    return route.fulfill({json:{ok:true,features:{},sessions:[],tools:[],messages:[],runs:[],events:[],workflows:[],schedules:[],prefs:{}}});
  });
  await page.goto("/");
  await page.getByRole("navigation").getByRole("button",{name:"Workflows",exact:true}).click();
  await page.getByText("conditional-progress",{exact:true}).first().click();
  const progress = page.getByRole("region",{name:"Process progress"});
  await expect(progress).toContainText("1 executed · 1 skipped");
  const task = page.getByTestId("workflow-task-optional");
  await expect(task.getByText("Skipped",{exact:true})).toBeVisible();
  await expect(task).toContainText("Action was not executed.");
  await expect(task.getByText("source/payload/accepted",{exact:true})).not.toBeVisible();
  await task.getByText("Condition did not match",{exact:true}).click();
  await expect(task.getByText("source/payload/accepted",{exact:true})).toBeVisible();
  await page.setViewportSize({width:320,height:800});
  expect(await progress.evaluate(e => e.scrollWidth <= e.clientWidth)).toBe(true);
  invalid = true;
  await page.getByTestId("workflow-detail-panel").getByRole("button",{name:"Reload",exact:true}).click();
  await expect(task.getByText("Skipped",{exact:true})).toHaveCount(0);
  await expect(progress).toContainText("1 queued");
  expect(writes).toBe(0);
});
