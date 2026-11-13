import {test,expect} from "@playwright/test";
test("resource ledger explains reserved exhausted and uncertain capacity without dispatch",async({page})=>{
 let state="held";const writes:string[]=[];
 const vector=(n:number)=>({max_tool_calls_total:n,max_steps_total:n,max_elapsed_ms_total:n,max_total_tokens:n});
 await page.addInitScript(()=>{localStorage.setItem("agentui.connectionMode",JSON.stringify("direct"));localStorage.setItem("agentui.base","http://127.0.0.1:7777");localStorage.setItem("agentui.showSettings","false");});
 await page.route("**/api/v1/**",r=>{if(r.request().method()!=="GET")writes.push(new URL(r.request().url()).pathname);return r.fulfill({json:{ok:true,features:{},sessions:[],tools:[],messages:[],runs:[],events:[],workflows:[],schedules:[],prefs:{}}});});
 await page.route("**/api/v1/workflows**",r=>r.fulfill({json:{ok:true,workflows:[{workflow_id:"resources",status:"running"}]}}));
 await page.route(/\/api\/v1\/workflow\?/,r=>r.fulfill({json:{ok:true,workflow:{workflow_id:"resources",status:state==="stopped"?"cancelled":"running",cancel_requested:state==="stopped"},tasks:[],
  workflow_limits:vector(10),workflow_remaining:vector(state==="stopped"?5:0),
  ...(state==="missing"?{}:{budget_accounting:{schema:"agentd.workflow.budget.v1",charged:vector(state==="held"?2:10),held:vector(state==="held"?8:0),uncertain:vector(state==="held"?0:4),active_attempts:state==="held"?1:0,uncertain_attempts:state==="held"?0:1}}),
  ...(state==="exhausted"?{result:{results_by_task:{model:{ok:false,total_tokens:3,token_budget_charge:10,token_usage_incomplete:true}}}}:{}),
 }}));
 await page.goto("/");await page.getByRole("navigation").getByRole("button",{name:"Workflows",exact:true}).click();
 await page.getByTestId("workflow-list-panel").getByText("resources",{exact:true}).click();
 const panel=page.getByTestId("workflow-budgets");
 await expect(panel).toContainText("Capacity reserved by active work");
 await expect(panel).not.toContainText("Budget exhausted:");
 await expect(panel.getByRole("region",{name:"Token budget",exact:true})).toContainText("Charged2");
 await expect(panel.getByRole("region",{name:"Token budget",exact:true})).toContainText("Held for active work8");
 await page.setViewportSize({width:320,height:780});expect(await panel.evaluate(e=>e.scrollWidth<=e.clientWidth)).toBe(true);
 state="exhausted";await page.getByRole("button",{name:"Reload",exact:true}).click();
 await expect(panel).toContainText("Budget exhausted:");
 await expect(panel).toContainText("Outcome uncertain for 1 lost attempt");
 await expect(panel).toContainText("Provider usage incomplete for model");
 await expect(panel).toContainText("already included in charged amounts");
 await expect(panel.getByRole("button")).toHaveCount(0);
 state="stopped";await page.getByRole("button",{name:"Reload",exact:true}).click();
 await expect(panel).toContainText("Remaining capacity does not resume a stopped or finished run");
 await expect(panel).not.toContainText("Provider usage incomplete for model");
 state="missing";await page.getByRole("button",{name:"Reload",exact:true}).click();
 await expect(panel).toContainText("Reservation accounting is unavailable");
 await expect(panel).not.toContainText("Budget exhausted:");
 await expect(panel).not.toContainText("Outcome uncertain");
 await expect(panel.getByRole("region",{name:"Token budget",exact:true})).toContainText("Available nowNot established");
 console.log("Unrelated app initialization writes:",writes);
 expect(writes.filter(path=>/workflow|run|job/.test(path))).toEqual([]);
});
