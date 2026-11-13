import {test,expect} from "@playwright/test";

for (const fetchMode of [false,true]) {
 test(`valid streamed completion finishes without fallback: fetch=${fetchMode}`,async({page})=>{
  let polls=0;
  await page.route("**/api/v1/**",r=>r.fulfill({json:{ok:true,sessions:[],features:{},tools:[],prefs:{}}}));
  await page.route("**/api/v1/job/stream?**",r=>r.fulfill({contentType:"text/event-stream",body:'event: job_done\ndata: {"status":"done","result":{"ok":true,"assistant_text":"stream-result"}}\n\n'}));
  await page.route("**/api/v1/job?**",r=>{polls++;return r.fulfill({json:{ok:true,status:"running"}});});
  await page.goto("/");await page.evaluate(async mode=>{(await import("/e2e/jobWatcherHarness.tsx")).mount(mode);},fetchMode);
  await expect.poll(()=>page.evaluate(()=>(window as any).watchTest?.result?.assistant_text)).toBe("stream-result");
  const state=await page.evaluate(()=>(window as any).watchTest);
  expect(state.active).toBe(null);expect(state.stored.scope).toBeUndefined();expect(polls).toBe(0);
 });
}

for (const fetchMode of [false,true]) {
 for (const payload of ["{}", '{"status":"running","result":{"ok":true}}', '{"status":"done"}', "not-json"]) {
  test(`invalid completion preserves original job then polls real result: fetch=${fetchMode} ${payload}`,async({page})=>{
   let terminal=false,polls=0;
   await page.route("**/api/v1/**",r=>r.fulfill({json:{ok:true,sessions:[],features:{},tools:[],prefs:{}}}));
   await page.route("**/api/v1/job/stream?**",r=>r.fulfill({contentType:"text/event-stream",body:"event: job_done\ndata: "+payload+"\n\n"}));
   await page.route("**/api/v1/job?**",r=>{polls++;return r.fulfill({json:{ok:true,job_id:"original",status:terminal?"done":"running",...(terminal?{result:{ok:true,assistant_text:"recovered"}}:{})}});});
   await page.goto("/");
   await page.evaluate(async(mode)=>{(await import("/e2e/jobWatcherHarness.tsx")).mount(mode);},fetchMode);
   await expect.poll(()=>polls).toBeGreaterThan(0);
   const state=await page.evaluate(()=>(window as any).watchTest);
   expect(state.active).toBe("original");expect(state.stored.scope.job_id).toBe("original");expect(state.result).toBeUndefined();
   terminal=true;
   await expect.poll(()=>page.evaluate(()=>(window as any).watchTest?.result?.assistant_text)).toBe("recovered");
   expect((await page.evaluate(()=>(window as any).watchTest)).active).toBe(null);
  });
 }
}
test("late watchdog completion cannot clear a newer job",async({page})=>{
 let release:()=>void=()=>{},held=false;
 const gate=new Promise<void>(r=>release=r);
 await page.route("**/api/v1/**",r=>r.fulfill({json:{ok:true,sessions:[],features:{},tools:[],prefs:{}}}));
 await page.route("**/api/v1/job/stream?**",r=>r.fulfill({contentType:"text/event-stream",body:""}));
 await page.route("**/api/v1/job?**",async r=>{
  const u=new URL(r.request().url()),id=u.searchParams.get("job_id");
  if(id==="original"&&!u.searchParams.has("include_events")){held=true;await gate;return r.fulfill({json:{ok:true,job_id:id,status:"done",result:{ok:true,assistant_text:"stale"}}});}
  return r.fulfill({json:{ok:true,job_id:id,status:"running"}});
 });
 await page.goto("/");await page.evaluate(async()=>{(await import("/e2e/jobWatcherHarness.tsx")).mount(true);});
 await expect.poll(()=>held,{timeout:7000}).toBe(true);
 await page.evaluate(()=>(window as any).watchTest.switchJob());release();
 await page.waitForTimeout(500);
 const state=await page.evaluate(()=>(window as any).watchTest);
 expect(state.active).toBe("new-job");expect(state.result).toBeUndefined();
});
