import { test, expect, type Page } from "@playwright/test";

const prefsURL = /\/(?:api\/v1\/client\/prefs|v1\/client_prefs)(?:\?|$)/;
const state = (page: Page) => page.evaluate(() => (window as any).runWatchTest);
const watches = (page: Page) => page.evaluate(() => JSON.parse(JSON.parse(localStorage.getItem("agentui.jobsBySession") || '"{}"')));
async function seed(page: Page, entries: object) {
  await page.evaluate(entries => localStorage.setItem("agentui.jobsBySession", JSON.stringify(JSON.stringify(entries))), entries);
}
async function mount(page: Page, options: { strict?: boolean; queue?: boolean; broker?: boolean } = {}) {
  await page.evaluate(async options => { (await import("/e2e/runWatchHarness.tsx")).mount(options); }, options);
}
async function setup(page: Page) {
  const runs: any[] = [], reads: string[] = [], uploads: any[] = [], terminal = new Set<string>();
  await page.route("**/api/v1/**", route => route.fulfill({json:{ok:true,sessions:[],features:{},tools:[],prefs:{}}}));
  await page.route(prefsURL, route => {
    if (route.request().method() === "POST") uploads.push(route.request().postDataJSON());
    return route.fulfill({json:{ok:true,found:true,client_id:"watch-fixture",client_kind:"run_watch",prefs:{}}});
  });
  await page.route("**/api/v1/run_async", route => {
    const request = route.request().postDataJSON();runs.push(request);
    return route.fulfill({json:{ok:true,job_id:`native-${request.session_id}-${runs.length}`,trace_id:request.trace_id}});
  });
  await page.route("**/api/v1/job/stream?**", route => route.fulfill({contentType:"text/event-stream",body:""}));
  await page.route("**/api/v1/job?**", route => {
    const job = new URL(route.request().url()).searchParams.get("job_id")!;reads.push(job);
    return route.fulfill({json:{ok:true,job_id:job,status:terminal.has(job)?"done":"running",
      ...(terminal.has(job)?{result:{ok:true,assistant_text:"original output"}}:{})}});
  });
  await page.goto("/");
  return { runs, reads, uploads, terminal };
}

for (const broker of [false, true]) {
  test(`delayed future-dated preferences cannot replace native acceptance: broker=${broker}`, async ({page}) => {
    const mock = await setup(page);
    let release!: () => void, loading = false;
    const gate = new Promise<void>(resolve => { release = resolve; });
    await page.route(prefsURL, async route => {
      if (route.request().method() === "POST") { mock.uploads.push(route.request().postDataJSON());return route.fulfill({json:{ok:true,found:true,client_id:"watch-fixture",client_kind:"run_watch"}}); }
      loading = true;await gate;
      await route.fulfill({json:{ok:true,found:true,client_id:"watch-fixture",client_kind:"run_watch",prefs:{run_watch:{by_scope:{"scope-a":{
        job_id:"stale-server-job",cursor:999,updated_unix_ms:Date.now()+86400000,local_state:"accepted",
      }}}}}}).catch(() => {});
    });
    try {
      await mount(page, {broker});
      await expect.poll(() => loading).toBe(true);
      await page.evaluate(() => (window as any).runWatchTest.send());
      await expect.poll(async () => (await state(page))?.active).toBe("native-scope-a-1");
      const received = page.waitForResponse(response => prefsURL.test(response.url()) && response.request().method()==="GET");
      release();await received;
      await expect.poll(async () => (await state(page))?.mode).toBe("server");
      expect((await watches(page))["scope-a"]).toMatchObject({job_id:"native-scope-a-1",local_state:"accepted",cursor:0});
      expect((await state(page)).active).toBe("native-scope-a-1");
      expect(mock.runs).toHaveLength(1);expect(mock.reads).not.toContain("stale-server-job");
      await expect.poll(() => mock.uploads.length).toBeGreaterThan(0);
      expect(mock.uploads[0].prefs.run_watch.by_scope["scope-a"]).toMatchObject({job_id:"native-scope-a-1"});
      expect(JSON.stringify(mock.uploads[0])).not.toContain("local_state");
    } finally { release(); }
  });
}

test("reload holds the queue until the original native job completes, including an old accepted pin", async ({page}) => {
  const mock = await setup(page);
  await seed(page, {"scope-a":{job_id:"original",local_state:"accepted",started_unix_ms:Date.now()-10*86400000}});
  await page.reload();await mount(page, {strict:true,queue:true});
  await expect.poll(async () => (await state(page))?.active).toBe("original");
  await expect.poll(() => mock.reads.includes("original")).toBe(true);
  expect(mock.runs).toHaveLength(0);expect((await state(page)).queue).toHaveLength(2);
  mock.terminal.add("original");
  await expect.poll(async () => (await state(page))?.active).toBe("native-scope-a-1");
  expect(mock.runs).toHaveLength(1);expect(mock.runs[0].prompt).toBe("queued first");
  expect((await state(page)).queue).toHaveLength(1);
});

test("verified completion retires the exact job and survives a stale preference reply and reload", async ({page}) => {
  const mock = await setup(page);
  await seed(page, {"scope-a":{job_id:"original",local_state:"accepted",started_unix_ms:Date.now()}});
  let release!: () => void;
  const gate = new Promise<void>(resolve => { release = resolve; });
  await page.route(prefsURL, async route => {
    if (route.request().method()==="POST") { mock.uploads.push(route.request().postDataJSON());return route.fulfill({json:{ok:true,found:true,client_id:"watch-fixture",client_kind:"run_watch"}}); }
    await gate;
    await route.fulfill({json:{ok:true,found:true,client_id:"watch-fixture",client_kind:"run_watch",prefs:{run_watch:{by_scope:{"scope-a":{
      job_id:"original",updated_unix_ms:Date.now()+86400000,
    }}}}}}).catch(() => {});
  });
  try {
    mock.terminal.add("original");await mount(page);
    await expect.poll(async () => (await watches(page))["scope-a"]?.local_state).toBe("retired");
    expect((await state(page)).result?.assistant_text).toBe("original output");
    release();await expect.poll(async () => (await state(page))?.mode).toBe("server");
    expect((await watches(page))["scope-a"].local_state).toBe("retired");
    await expect.poll(() => mock.uploads.some(upload => !upload.prefs.run_watch.by_scope["scope-a"])).toBe(true);
    const reads = mock.reads.length;
    await page.reload();await mount(page, {strict:true});
    await expect.poll(async () => (await state(page))?.mode).toBe("server");
    expect((await state(page)).active).toBe(null);expect(mock.reads).toHaveLength(reads);expect(mock.runs).toHaveLength(0);
  } finally { release(); }
});

test("independent accepted scopes survive delayed reconciliation and switching", async ({page}) => {
  const mock = await setup(page);
  let release!: () => void;
  const gate = new Promise<void>(resolve => { release = resolve; });
  await page.route(prefsURL, async route => {
    if (route.request().method()==="POST") return route.fulfill({json:{ok:true,found:true,client_id:"watch-fixture",client_kind:"run_watch"}});
    await gate;
    await route.fulfill({json:{ok:true,found:true,client_id:"watch-fixture",client_kind:"run_watch",prefs:{run_watch:{by_scope:Object.fromEntries(["scope-a","scope-b"].map(scope=>[
      scope,{job_id:"old-"+scope,updated_unix_ms:Date.now()+86400000},
    ]))}}}}).catch(() => {});
  });
  try {
    await mount(page);await page.evaluate(() => (window as any).runWatchTest.send());
    await expect.poll(async () => (await state(page))?.active).toBe("native-scope-a-1");
    await page.evaluate(() => (window as any).runWatchTest.setScope("scope-b"));
    await expect.poll(async () => (await state(page))?.active).toBe(null);
    await page.evaluate(() => (window as any).runWatchTest.send());
    await expect.poll(async () => (await state(page))?.active).toBe("native-scope-b-2");
    release();await expect.poll(async () => (await state(page))?.mode).toBe("server");
    expect((await watches(page))["scope-a"].job_id).toBe("native-scope-a-1");
    expect((await watches(page))["scope-b"].job_id).toBe("native-scope-b-2");
    mock.terminal.add("native-scope-a-1");
    await page.evaluate(() => (window as any).runWatchTest.setScope("scope-a"));
    await expect.poll(async () => (await watches(page))["scope-a"]?.local_state).toBe("retired");
    expect((await watches(page))["scope-b"]).toMatchObject({job_id:"native-scope-b-2",local_state:"accepted"});
    await page.evaluate(() => (window as any).runWatchTest.setScope("scope-b"));
    await expect.poll(async () => (await state(page))?.active).toBe("native-scope-b-2");
    expect(mock.runs).toHaveLength(2);expect(mock.reads.some(id=>id.startsWith("old-"))).toBe(false);
  } finally { release(); }
});

test("remote hints cannot promote authority; exact retirement and cache bounds remain conservative", async ({page}) => {
  await setup(page);
  const receipt = await page.evaluate(async () => {
    const prefs = await import("/src/runWatchPrefs.ts"), store = await import("/src/jobStore.ts");
    const now = Date.now();
    const parsed = prefs.parseRunWatchByScope({remote:{job_id:"hint",local_state:"accepted",retired_unix_ms:now},invalid:{job_id:42}});
    const retired = prefs.retireJobWatch({scope:{job_id:"new",local_state:"accepted"}},"scope","old");
    const pins = Object.fromEntries(Array.from({length:64},(_,i)=>["pin-"+i,{job_id:"job-"+i,local_state:"accepted",started_unix_ms:1}]));
    store.writeJobWatches(pins);
    const before = localStorage.getItem("agentui.jobsBySession");
    let capacityHeld = false;
    try { store.persistAcceptedJobWatch("overflow","overflow"); } catch { capacityHeld = true; }
    const unchanged = before === localStorage.getItem("agentui.jobsBySession");
    const expired = store.pruneJobsBySession(now,{retired:{job_id:"old",local_state:"retired",retired_unix_ms:now-8*86400000},pin:{job_id:"pin",local_state:"accepted",started_unix_ms:1}}).next;
    const recent = store.pruneJobsBySession(now,{retired:{job_id:"r",local_state:"retired",started_unix_ms:1,retired_unix_ms:now},hint:{job_id:"h",started_unix_ms:now+86400000}},1).next;
    return {parsed,retired,capacityHeld,unchanged,expired,recent};
  });
  expect(receipt.parsed).toEqual({remote:{job_id:"hint"}});
  expect(receipt.retired.scope).toMatchObject({job_id:"new",local_state:"accepted"});
  expect(receipt.capacityHeld).toBe(true);expect(receipt.unchanged).toBe(true);
  expect(Object.keys(receipt.expired)).toEqual(["pin"]);expect(Object.keys(receipt.recent)).toEqual(["retired"]);
});

for (const fault of ["invalid", "unavailable"] as const) {
  test(`saved watch ${fault} storage holds automatic dispatch`, async ({page}) => {
    const mock = await setup(page);
    await page.evaluate(fault => {
      if (fault === "invalid") localStorage.setItem("agentui.jobsBySession", JSON.stringify(JSON.stringify({"scope-a":{job_id:null}})));
      else {
        const get = Storage.prototype.getItem;
        Storage.prototype.getItem = function(key) { if (key==="agentui.jobsBySession") throw new Error("fixture storage unavailable");return get.call(this,key); };
      }
    }, fault);
    await mount(page, {queue:true});
    await expect.poll(async () => (await state(page))?.mode).toBe("local");
    expect((await state(page)).active).toBe(null);expect((await state(page)).queue).toHaveLength(2);
    expect(mock.runs).toHaveLength(0);
  });
}

test("authorization changes abort old preference reads and writes without importing their late replies", async ({page}) => {
  const mock = await setup(page);
  await seed(page, {"scope-a":{job_id:"original",local_state:"accepted",started_unix_ms:Date.now()}});
  let release!: () => void, oldReads = 0, oldWrites = 0;
  const gate = new Promise<void>(resolve => { release = resolve; });
  const requests: {client:string;method:string}[] = [];
  await page.route(prefsURL, async route => {
    const method = route.request().method();
    const client = method === "POST" ? route.request().postDataJSON().client_id : new URL(route.request().url()).searchParams.get("client_id")!;
    requests.push({client,method});
    if (client === "watch-fixture") {
      if (method==="POST") oldWrites++;else oldReads++;
      await gate;
      await route.fulfill({json:{ok:true,found:true,client_id:"watch-fixture",client_kind:"run_watch",prefs:{run_watch:{by_scope:{"old-only":{job_id:"must-not-import"}}}}}}).catch(() => {});
    } else await route.fulfill({json:{ok:true,found:true,client_id:"watch-fixture",client_kind:"run_watch",prefs:{}}});
  });
  try {
    await mount(page);
    await expect.poll(() => oldReads).toBe(1);
    await page.evaluate(() => (window as any).runWatchTest.touch());
    await expect.poll(() => oldWrites).toBe(1);
    // This update is coalesced behind the old POST; changing the authority must
    // dispose it instead of replaying that old request against new credentials.
    await page.evaluate(() => {
      (window as any).runWatchTest.touch();
      (window as any).runWatchTest.changePrefs({client:"new-client",token:"new-fixture",base:location.origin+"/new-authority"});
    });
    await expect.poll(() => requests.some(request=>request.client==="new-client" && request.method==="POST")).toBe(true);
    release();
    await expect.poll(async () => (await state(page))?.mode).toBe("server");
    expect((await watches(page))["old-only"]).toBeUndefined();
    expect((await watches(page))["scope-a"]).toMatchObject({job_id:"original",local_state:"accepted",cursor:2});
    expect(oldWrites).toBe(1);expect(mock.runs).toHaveLength(0);
  } finally { release(); }
});

test("metadata writes serialize and coalesce using a monotonic clock after wall-clock rollback", async ({page}) => {
  const mock = await setup(page);
  await page.clock.install();
  await seed(page, {"scope-a":{job_id:"original",local_state:"accepted",started_unix_ms:Date.now()}});
  let release!: () => void;
  const gate = new Promise<void>(resolve => { release = resolve; });
  await page.route(prefsURL, async route => {
    if (route.request().method()==="GET") return route.fulfill({json:{ok:true,found:true,client_id:"watch-fixture",client_kind:"run_watch",prefs:{}}});
    mock.uploads.push(route.request().postDataJSON());
    if (mock.uploads.length===1) await gate;
    await route.fulfill({json:{ok:true,found:true,client_id:"watch-fixture",client_kind:"run_watch"}}).catch(() => {});
  });
  try {
    await mount(page);
    await expect.poll(() => mock.uploads.length).toBe(1);
    await page.evaluate(() => { (window as any).runWatchTest.touch();(window as any).runWatchTest.touch(); });
    expect(mock.uploads).toHaveLength(1);
    await page.clock.setSystemTime(Date.now()-86400000);
    const response = page.waitForResponse(response => prefsURL.test(response.url()) && response.request().method()==="POST");
    release();await response;
    await page.clock.fastForward(5100);
    await expect.poll(() => mock.uploads.length).toBe(2);
    expect(mock.uploads[1].prefs.run_watch.by_scope["scope-a"]).toMatchObject({job_id:"original",cursor:2});
    expect(mock.runs).toHaveLength(0);
  } finally { release(); }
});

test("completion storage failure preserves the original watch and holds queued work until retirement saves", async ({page}) => {
  const mock = await setup(page);
  await seed(page, {"scope-a":{job_id:"original",local_state:"accepted",started_unix_ms:Date.now()}});
  await mount(page, {queue:true});
  await expect.poll(() => mock.reads.includes("original")).toBe(true);
  await page.evaluate(() => {
    const set = Storage.prototype.setItem;
    (window as any).restoreWatchStorage = () => { Storage.prototype.setItem = set; };
    Storage.prototype.setItem = function(key,value) {
      if (key==="agentui.jobsBySession") throw new Error("fixture quota exceeded");
      set.call(this,key,value);
    };
  });
  mock.terminal.add("original");
  await expect.poll(async () => (await state(page))?.notice?.includes("unverified")).toBe(true);
  expect((await state(page)).active).toBe("original");expect((await state(page)).queue).toHaveLength(2);
  expect((await watches(page))["scope-a"].local_state).toBe("accepted");expect(mock.runs).toHaveLength(0);
  await page.evaluate(() => (window as any).restoreWatchStorage());
  await expect.poll(async () => (await state(page))?.active).toBe("native-scope-a-1");
  expect(mock.runs).toHaveLength(1);expect(mock.runs[0].prompt).toBe("queued first");
});
