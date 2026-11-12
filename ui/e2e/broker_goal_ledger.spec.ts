import { expect, test, type Page } from "@playwright/test";
import { seedBrokerState } from "./brokerTestState";

const binding = { agent_id: "agent1", deployment_id: "dep1", session_id: "work-session" };
const goal = (id: string, title = id) => ({
  orchestrator_run_id: id, team_id: "team-alpha", status: "waiting", goal: title,
  meta: { work_ledger: binding, orchestrator_owner: "worker", work_ledger_runtime: {
    reason: "requirements_blocked", managed_wait: true, revision: 4,
    requirements: [{ id: "build", title: "Build product", status: "done", runnable: false },
      { id: "release", title: "Release product", status: "pending", runnable: false }],
  } },
});
async function setup(page: Page) {
  await seedBrokerState(page);
  await page.route("**/v1/**", async (route, request) => {
    const path = new URL(request.url()).pathname;
    if (path === "/v1/agents") return route.fulfill({ json: { ok: true, agents: [{
      agent_id: "agent1", display_name: "Agent One", created_unix_ms: 1700000000000, enabled: true, connected: true, owner_sub: "owner", deployments: [{ deployment_id: "dep1", connected: true }],
    }] } });
    if (path === "/v1/teams") return route.fulfill({ json: { ok: true, teams: [
      { team_id: "team-alpha", display_name: "Alpha", owner_sub: "owner", created_unix_ms: 1700000000000 }, { team_id: "team-beta", display_name: "Beta", owner_sub: "owner", created_unix_ms: 1700000000000 },
    ] } });
    if (/^\/v1\/teams\/team-(alpha|beta)$/.test(path)) return route.fulfill({ json: { ok: true, team: { team_id: path.split("/").pop(), display_name: "Team", owner_sub: "owner", created_unix_ms: 1700000000000, meta: {} } } });
    if (path.endsWith("/orchestrator/runs")) return route.fulfill({ json: { ok: true, team_id: path.split("/")[3], runs: [goal("goal-a", "First goal"), goal("goal-b", "Second goal")] } });
    if (path.includes("/orchestrator/runs/")) return route.fulfill({ json: { ok: true, team_id: "team-alpha", run: goal(path.split("/").pop()!) } });
    return route.fulfill({ json: { ok: true, found: false, prefs: {}, version: 1, agent_id: "agent1", owner_sub: "owner",
      members: [], audit: [], deployments: [{ deployment_id: "dep1", connected: true }], events: [], runs: [], rules: [], connectors: [],
      sessions: [], tools: [], workflows: [], entities: [], messages: [], guidance: [], spawn_requests: [], approvals: [] } });
  });
}
async function open(page: Page) {
  await page.goto("/");
  await page.getByRole("button", { name: "Broker Console" }).click();
  const broker = page.locator("details").filter({ has: page.getByText("Broker panel", { exact: true }) });
  await broker.getByRole("button", { name: "Teams", exact: true }).click();
  await broker.getByRole("button", { name: "Refresh" }).first().click();
  await expect(page.getByTestId("team-select")).toHaveValue("team-alpha");
  await page.getByTestId("team-tab-advanced").click();
  if (!(await page.getByTestId("orchestrator-run-panel").isVisible())) {
    await page.locator("summary").filter({ has: page.getByText("Orchestrator runs", { exact: true }) }).first().click();
  }
  await expect(page.getByTestId("orchestrator-run-panel")).toBeVisible();
}

test("bound goal creation validates exact authority and repeated clicks create one goal", async ({ page }) => {
  await setup(page);
  let creates = 0, release!: () => void;
  const gate = new Promise<void>((resolve) => { release = resolve; });
  await page.route("**/v1/teams/team-alpha/orchestrator/runs", async (route, request) => {
    if (request.method() !== "POST") return route.fallback();
    creates++; expect(request.postDataJSON().meta.work_ledger).toEqual(binding);
    await gate;
    return route.fulfill({ json: { ok: true, team_id: "team-alpha", run: goal("created", "Created goal") } });
  });
  await open(page);
  await page.getByTestId("orchestrator-run-create-goal").fill("Deliver product");
  await page.getByTestId("goal-ledger-enabled").check();
  await page.getByTestId("orchestrator-run-create").click();
  await expect(page.getByText("Requirements need valid agent, deployment and session IDs.", { exact: true })).toBeVisible();
  expect(creates).toBe(0);
  for (const [key, value] of Object.entries(binding)) await page.getByTestId("goal-ledger-" + key).fill(value);
  await page.getByTestId("orchestrator-run-create").evaluate((button) => { (button as HTMLButtonElement).click(); (button as HTMLButtonElement).click(); });
  await expect.poll(() => creates).toBe(1);
  release();
  await expect(page.getByTestId("goal-ledger-count")).toHaveText("1 of 2 requirements verified · observed revision 4");
  await expect(page.getByTestId("goal-ledger-progress")).toContainText("Waiting on affected requirements");
  await expect(page.getByTestId("goal-requirement-release")).toHaveText("Waiting: Release product");
});

test("owner pause carries the observed metadata guard and stays visibly paused", async ({ page }) => {
  await setup(page);
  let updates = 0;
  await page.route("**/orchestrator/runs/goal-a", async (route, request) => {
    if (request.method() !== "PATCH") return route.fallback();
    updates++;
    const body = request.postDataJSON();
    expect(body.expected_meta).toEqual(goal("goal-a").meta);
    expect(body.expected_status).toBe("waiting");
    expect(body.status).toBe("paused");
    return route.fulfill({ json: { ok: true, team_id: "team-alpha", run: { ...goal("goal-a"), status: "paused" } } });
  });
  await open(page);
  await page.getByTestId("orchestrator-run-row-goal-a").click();
  await expect(page.getByTestId("goal-ledger-count")).toBeVisible();
  await page.getByTestId("orchestrator-run-update-status").fill("paused");
  await page.getByTestId("orchestrator-run-update").click();
  await expect(page.getByTestId("goal-ledger-status")).toHaveText("Paused by owner");
  expect(updates).toBe(1);
});

test("late requirement state cannot cross goal selection or a team boundary", async ({ page }) => {
  await setup(page);
  let release!: () => void, requested = false;
  const gate = new Promise<void>((resolve) => { release = resolve; });
  await page.route("**/v1/teams/team-alpha/orchestrator/runs/goal-a", async (route) => {
    requested = true; await gate;
    return route.fulfill({ json: { ok: true, team_id: "team-alpha", run: goal("goal-a", "Old private goal") } }).catch(() => {});
  });
  await open(page);
  await page.getByTestId("orchestrator-run-row-goal-a").click();
  await expect.poll(() => requested).toBe(true);
  await page.getByTestId("orchestrator-run-row-goal-b").click();
  await expect(page.getByTestId("orchestrator-run-id")).toHaveValue("goal-b");
  await expect(page.getByTestId("goal-ledger-count")).toBeVisible();
  await page.getByTestId("team-select").selectOption("team-beta");
  await expect(page.getByTestId("goal-ledger-progress")).toHaveCount(0);
  release();
  await expect(page.getByText("Old private goal", { exact: true })).toHaveCount(0);
  await page.getByTestId("orchestrator-run-row-goal-b").click();
  await expect(page.getByTestId("goal-ledger-count")).toBeVisible();
});
