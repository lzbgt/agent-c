import { expect, test } from "@playwright/test";

test("run diff panel compares replay bundles and DB evidence", async ({ page }) => {
  await page.addInitScript(() => {
    try {
      window.localStorage.setItem("agentui.simpleMode", "false");
      window.localStorage.setItem("agentui.connectionMode", JSON.stringify("direct"));
      window.localStorage.setItem("agentui.base", "http://127.0.0.1:7788");
      window.localStorage.setItem("agentui.showSettings", "false");
    } catch {
      // ignore
    }
  });

  await page.route("http://127.0.0.1:7788/api/v1/**", async (route, request) => {
    const url = new URL(request.url());
    const path = url.pathname;
    const runId = url.searchParams.get("run_id");

    if (path === "/api/v1/run/replay") {
      const body =
        runId === "101"
          ? {
              ok: true,
              run_id: 101,
              replay_sha256: "hash-a",
              bundle: {
                schema: "replay.v1",
                request: { prompt: "hello", system: "baseline" },
                response: { assistant_text: "A", usage: { total_tokens: 10 } },
                tool_records: [{ tool: "shell", args: "pwd" }],
              },
            }
          : {
              ok: true,
              run_id: 202,
              replay_sha256: "hash-b",
              bundle: {
                schema: "replay.v1",
                request: { prompt: "hello world", system: "candidate" },
                response: { assistant_text: "B", usage: { total_tokens: 16 } },
                tool_records: [{ tool: "shell", args: "ls" }],
              },
            };
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify(body) });
      return;
    }

    if (path === "/api/v1/run/attestation") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          run_id: Number(runId),
          attestation: {
            schema: "attestation.v1",
            replay_sha256: runId === "101" ? "hash-a" : "hash-b",
          },
        }),
      });
      return;
    }

    if (path === "/api/v1/db/run") {
      const body =
        runId === "101"
          ? {
              ok: true,
              run: {
                run_id: 101,
                replay_sha256: "hash-a",
                response_json: JSON.stringify({ usage: { total_tokens: 12 } }),
              },
              events: [
                { event_id: "ev-1", type: "assistant", data_json: JSON.stringify({ text: "A" }) },
                { event_id: "ev-2", type: "tool", data_json: JSON.stringify({ tool: "shell", status: "ok" }) },
              ],
              artifacts: [
                { path: "/tmp/a.txt", kind: "text", title: "baseline", artifact_json: JSON.stringify({ content: "A" }) },
              ],
            }
          : {
              ok: true,
              run: {
                run_id: 202,
                replay_sha256: "hash-b",
                response_json: JSON.stringify({ usage: { total_tokens: 18 } }),
              },
              events: [
                { event_id: "ev-1", type: "assistant", data_json: JSON.stringify({ text: "B" }) },
                { event_id: "ev-3", type: "tool", data_json: JSON.stringify({ tool: "shell", status: "changed" }) },
                { event_id: "ev-4", type: "artifact", data_json: JSON.stringify({ artifact: "new" }) },
              ],
              artifacts: [
                { path: "/tmp/a.txt", kind: "text", title: "candidate", artifact_json: JSON.stringify({ content: "B" }) },
                { path: "/tmp/new.txt", kind: "text", title: "new", artifact_json: JSON.stringify({ content: "new" }) },
              ],
            };
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify(body) });
      return;
    }

    await route.fallback();
  });

  await page.goto("/");

  await page.getByRole("button", { name: "Run Diff" }).click();
  await expect(page.getByTestId("run-diff-panel")).toBeVisible();

  await page.getByTestId("run-diff-run-a-input").fill("101");
  await page.getByTestId("run-diff-run-b-input").fill("202");
  await page.getByRole("button", { name: "Load A" }).click();
  await page.getByRole("button", { name: "Load B" }).click();

  const loadersSection = page.getByTestId("run-diff-loaders-section");
  const replaySection = page.getByTestId("run-diff-replay-section");
  await expect(loadersSection).toContainText("replay hash differs");
  await expect(replaySection).toContainText("prompt");
  await expect(replaySection).toContainText("hello world");
  await expect(page.getByText("usage delta: total_tokens +6")).toBeVisible();

  await page.getByRole("button", { name: "Load evidence" }).first().click();
  await page.getByRole("button", { name: "Load evidence" }).nth(1).click();

  const evidenceSection = page.getByTestId("run-diff-evidence-section");
  await expect(evidenceSection).toContainText("event type deltas");
  await expect(evidenceSection).toContainText("artifacts added");
  await expect(evidenceSection).toContainText("/tmp/new.txt");
  await expect(evidenceSection).toContainText("artifacts changed");
  await expect(page.getByText("db usage delta: total_tokens +6")).toBeVisible();
});
