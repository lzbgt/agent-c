import { expect, test } from "@playwright/test";
import { execFileSync } from "node:child_process";

test("guided signal authoring runs exact pinned native SOP without implicit effects", async ({ page }) => {
  let submitted: any;
  let writes = 0;
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
    writes++; submitted=route.request().postDataJSON();
    return route.fulfill({json:{ok:true,workflow_id:"authored-signal"}});
  });
  await page.goto("/");
  await page.getByRole("navigation").getByRole("button",{name:"Workflows",exact:true}).click();
  await page.getByRole("button",{name:"Create workflow",exact:true}).click();
  await page.getByRole("combobox",{name:"Workflow template",exact:true}).selectOption("sop");
  await page.getByLabel("Add action",{exact:true}).first().selectOption("signal");
  await expect(page.getByRole("alert").filter({hasText:"Choose a signal name"})).toBeVisible();
  await page.getByLabel("Signal name",{exact:true}).fill("inspection.finished");
  await page.getByLabel("Signal label",{exact:true}).fill("Inspection ready");
  await page.getByLabel("Signal expiry (milliseconds)",{exact:true}).fill("10000");
  const early=page.getByRole("checkbox",{name:"Accept input before this action is ready",exact:true});
  await expect(early).not.toBeChecked();
  await early.check(); await early.uncheck(); await early.check();
  await page.getByText("Advanced process configuration",{exact:true}).click();
  const raw=page.getByTestId("workflow-composer-json");
  const draft=JSON.parse(await raw.inputValue());
  const signal=draft.sop.stages.flatMap((s:any)=>s.actions).find((a:any)=>a.kind==="signal");
  signal.max_attempts=2;
  draft.sop.stages.unshift({id:"prepare",actions:[{task_id:"prepare",kind:"delay",delay_ms:500}]});
  await raw.fill(JSON.stringify(draft));
  await page.getByLabel("Signal label",{exact:true}).fill("Inspection ready");
  expect(JSON.parse(await raw.inputValue()).sop.stages.flatMap((s:any)=>s.actions).find((a:any)=>a.kind==="signal").max_attempts).toBe(2);
  await page.setViewportSize({width:320,height:800});
  expect(await page.locator("#workspace-main").evaluate(e=>e.scrollWidth<=e.clientWidth)).toBe(true);
  expect(writes).toBe(0);expect(submitted).toBeUndefined();
  await page.getByRole("button",{name:"Submit workflow",exact:true}).click();
  await expect.poll(()=>!!submitted).toBe(true);
  expect(writes).toBe(1);
  expect(submitted.sop.stages.flatMap((s:any)=>s.actions).find((a:any)=>a.kind==="signal").signal.accept_before_activation).toBe(true);
  const output=execFileSync("python3",["../tests/workflow_authored_signal_smoke.py",process.env.AGENT_SIGNAL_TEST_BINARY || "../build-snapshot/agentd"],{
    input:JSON.stringify(submitted),encoding:"utf8",timeout:45000,
  });
  expect(output).toContain("PASS exact browser-authored signal SOP");
});

test("signal progress is read-only, bounded and keeps delivery identities collapsed",async({page})=>{
  let phase="waiting",writes=0;
  await page.addInitScript(()=>{
    localStorage.setItem("agentui.simpleMode","false");localStorage.setItem("agentui.workflowPanelOpen","true");
    localStorage.setItem("agentui.connectionMode",JSON.stringify("direct"));localStorage.setItem("agentui.base","http://127.0.0.1:7777");
    localStorage.setItem("agentui.showSettings","false");
  });
  await page.route("**/api/v1/**",route=>{
    const path=new URL(route.request().url()).pathname;
    if(path.startsWith("/api/v1/workflow")&&route.request().method()!=="GET")writes++;
    const workflow={workflow_id:"signal-progress",status:phase==="stopped"?"cancelled":phase==="resolved"?"done":phase==="expired"?"error":"running"};
    if(path==="/api/v1/workflows")return route.fulfill({json:{ok:true,workflows:[workflow]}});
    if(path==="/api/v1/workflow")return route.fulfill({json:{ok:true,workflow,tasks:[],signals:[{
      workflow_id:"signal-progress",task_id:"external-result",signal_id:"ws_exact-identity",request_hash:"sha256:source",
      name:"inspection.finished",title:"Inspection result",state:phase,available:phase==="waiting"||phase==="pending",created_unix_ms:1000,
      ...(["resolved","buffered","unused"].includes(phase)?{payload_hash:"sha256:accepted"}:{}),
    }]}});
    return route.fulfill({json:{ok:true,features:{},sessions:[],tools:[],messages:[],runs:[],events:[],workflows:[],schedules:[],prefs:{}}});
  });
  await page.goto("/");await page.getByRole("navigation").getByRole("button",{name:"Workflows",exact:true}).click();
  await page.getByText("signal-progress",{exact:true}).first().click();
  const card=page.getByRole("region",{name:"External signals"});
  await expect(card).toContainText("Waiting for input");
  await expect(card.getByText("ws_exact-identity",{exact:true})).not.toBeVisible();
  await card.getByText("Delivery details",{exact:true}).click();
  await expect(card.getByText("ws_exact-identity",{exact:true})).toBeVisible();
  for(const [state,label] of [["pending","Accepting early input"],["buffered","Input saved; waiting for earlier steps"],["unused","Action did not run"],["resolved","Input received"],["expired","Expired without input"],["stopped","Stopped"]]){
    phase=state;await page.getByTestId("workflow-detail-panel").getByRole("button",{name:"Reload",exact:true}).click();
    await expect(card).toContainText(label);
  }
  await page.setViewportSize({width:320,height:800});
  expect(await card.evaluate(e=>e.scrollWidth<=e.clientWidth)).toBe(true);
  expect(writes).toBe(0);
});
