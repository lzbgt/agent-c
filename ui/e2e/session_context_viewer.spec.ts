import { expect, test } from "@playwright/test";
import { seedBrokerState } from "./brokerTestState";
test("actual history panel lazily pages context and never mixes changed generations", async ({ page }) => {
  const base = "https://broker.example.invalid", agentId = "context-native";
  await seedBrokerState(page, { agentId, brokerBase: base, brokerToken: "test-token", showSettings: false, brokerPanelOpen: false });
  await page.addInitScript(({ key }) => {
    localStorage.setItem("agentui.sessionByScope", JSON.stringify(JSON.stringify({ [key]: "context-session" })));
  }, { key: "profile:profile-broker-test::base:" + base + "/v1/agents/" + agentId + "/proxy" });
  let reads = 0, stale = false;
  await page.route("**/*", async route => {
    const url = new URL(route.request().url());
    if (!url.href.startsWith(base)) return route.continue();
    if (url.searchParams.has("messages_limit")) {
      reads++;
      expect(url.searchParams.get("messages_limit")).toBe("50");
      if (url.searchParams.has("messages_before_index")) {
        expect(url.searchParams.get("messages_revision")).toBe("7");
        if (stale) return route.fulfill({ status: 409, json: { error: "changed" } });
        return route.fulfill({ json: { ok: true, session_id: "context-session",
          messages: [{ index: 0, role: "system", content: "old context only" }],
          page: { revision: 7, has_more: true, next_before_index: 0 } } });
      }
      return route.fulfill({ json: { ok: true, session_id: "context-session",
        messages: [{ index: 50, role: "user", content: "latest context only" }],
        page: { revision: 7, has_more: true, next_before_index: 50 } } });
    }
    return route.fulfill({ json: { ok: true, session_id: "context-session",
      agents: [{ agent_id: agentId, connected: true }], sessions: ["context-session"],
      messages: [{ role: "user", content: "persisted chat stays" }], runs: [], events: [],
      features: {}, tools: [], prefs: {}, entities: [], artifacts: [] } });
  });
  await page.goto("/");
  await page.getByRole("navigation").getByRole("button", { name: "Chat", exact: true }).click();
  const viewer = page.getByTestId("session-context-pages");
  await expect(viewer).toBeVisible();
  expect(reads).toBe(0);
  await viewer.locator("summary").first().click();
  await expect(viewer.getByText("latest context only", { exact: true })).toBeVisible();
  await viewer.getByRole("button", { name: "Older context", exact: true }).click();
  await expect(viewer.getByText("old context only", { exact: true })).toBeVisible();
  await expect(viewer.getByText("latest context only", { exact: true })).toHaveCount(0);
  stale = true;
  await viewer.getByRole("button", { name: "Older context", exact: true }).click();
  await expect(viewer.getByRole("alert")).toContainText("Context changed");
  await expect(viewer.getByText("old context only", { exact: true })).toHaveCount(0);
  await expect(viewer.getByRole("button", { name: "Older context", exact: true })).toBeDisabled();
  await viewer.getByRole("button", { name: "Load latest context", exact: true }).click();
  await expect(viewer.getByText("latest context only", { exact: true })).toBeVisible();
});
