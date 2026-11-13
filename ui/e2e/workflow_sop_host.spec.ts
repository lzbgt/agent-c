import {test,expect} from "@playwright/test";
import {execFileSync} from "node:child_process";
test("guided host command preserves argv and native policy",async({page})=>{
 let submitted:any;
 await page.addInitScript(()=>{localStorage.setItem("agentui.connectionMode",JSON.stringify("direct"));localStorage.setItem("agentui.base","http://127.0.0.1:7777");localStorage.setItem("agentui.showSettings","false");});
 await page.route("**/api/v1/**",r=>r.fulfill({json:{ok:true,features:{},sessions:[],tools:[],messages:[],runs:[],events:[],workflows:[],schedules:[],prefs:{}}}));
 await page.route("**/api/v1/workflow/submit",r=>{submitted=r.request().postDataJSON();return r.fulfill({json:{ok:true,workflow_id:"host-authored"}});});
 await page.goto("/");await page.getByRole("navigation").getByRole("button",{name:"Workflows",exact:true}).click();
 await page.getByRole("button",{name:"Create workflow",exact:true}).click();
 await page.getByRole("combobox",{name:"Workflow template",exact:true}).selectOption("sop");
 await page.getByLabel("Add action",{exact:true}).first().selectOption("host_tool");
 await expect(page.getByRole("alert").filter({hasText:"Choose an executable"})).toBeVisible();
 await page.getByLabel("Executable",{exact:true}).fill("/usr/bin/printf");
 for(const value of ["[%s]","space ; $(literal)","","remove me"]){
  await page.getByRole("button",{name:"Add command argument",exact:true}).click();
  const n=await page.getByLabel(/^Argument [0-9]+$/).count();
  await page.getByLabel("Argument "+n,{exact:true}).fill(value);
 }
 await page.getByRole("button",{name:"Remove argument 4",exact:true}).click();
 await page.getByLabel("Host command timeout (milliseconds)",{exact:true}).fill("4500");
 await page.getByText("Advanced process configuration",{exact:true}).click();
 const raw=page.getByTestId("workflow-composer-json");let draft=JSON.parse(await raw.inputValue());
 const action=draft.sop.stages[0].actions.find((a:any)=>a.kind==="host_tool");
 action.max_attempts=1;action.host_tool.arguments.max_output_bytes=4096;
 await raw.fill(JSON.stringify(draft));
 await page.getByLabel("Executable",{exact:true}).fill("/usr/bin/printf");
 draft=JSON.parse(await raw.inputValue());
 expect(draft.sop.stages[0].actions.find((a:any)=>a.kind==="host_tool").host_tool.arguments.max_output_bytes).toBe(4096);
 const commandDraft=await raw.inputValue();
 // Unsupported tool configurations remain untouched, never silently converted.
 const advanced=JSON.parse(commandDraft),other=advanced.sop.stages[0].actions.find((a:any)=>a.kind==="host_tool");
 other.host_tool={name:"fs_read",arguments:{path:"preserved.txt"}};
 await raw.fill(JSON.stringify(advanced));
 await expect(page.getByLabel("Executable",{exact:true})).toHaveCount(0);
 await page.getByLabel("Process name",{exact:true}).fill("Preserve advanced tool");
 expect(JSON.parse(await raw.inputValue()).sop.stages[0].actions.find((a:any)=>a.kind==="host_tool").host_tool).toEqual(other.host_tool);
 await raw.fill(commandDraft);
 await page.setViewportSize({width:320,height:800});expect(await page.locator("#workspace-main").evaluate(e=>e.scrollWidth<=e.clientWidth)).toBe(true);
 expect(submitted).toBeUndefined();
 await page.getByRole("button",{name:"Submit workflow",exact:true}).click();await expect.poll(()=>!!submitted).toBe(true);
 const output=execFileSync("python3",["../tests/workflow_authored_host_smoke.py","../build/agentd"],{input:JSON.stringify(submitted),encoding:"utf8",timeout:45000});
 expect(output).toContain("PASS exact browser-authored host SOP");
});
