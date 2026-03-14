import { expect, test } from "@playwright/test";
import { seedBrokerState } from "./brokerTestState";

const agentId = "codexw-lab";
const brokerBase = "https://broker.example.invalid";
const proxyBase = `${brokerBase}/v1/agents/${agentId}/proxy`;
const sessionId = "sess-ops";
const sessionScopeKey = `profile:profile-broker-test::base:${proxyBase}`;

async function seedSessionScope(page: Parameters<typeof seedBrokerState>[0]) {
  await page.addInitScript(({ key, sid }) => {
    try {
      window.localStorage.setItem("agentui.sessionByScope", JSON.stringify({ [key]: sid }));
      window.localStorage.setItem("agentui.sessionLeaseSeconds", JSON.stringify("120"));
    } catch {
      // ignore storage failures in test bootstrap
    }
  }, { key: sessionScopeKey, sid: sessionId });
}

test("broker codexw operator section exposes orchestration shells services and capabilities", async ({ page }) => {
  await seedBrokerState(page, { agentId, brokerBase, brokerToken: "test-token", showSettings: true });
  await seedSessionScope(page);

  const shellState = {
    "bg-1": {
      ok: true,
      job: {
        job_id: "bg-1",
        label: "build",
        status: "running",
        stdout: "initial shell output\n",
      },
    },
    "bg-2": {
      ok: true,
      job: {
        job_id: "bg-2",
        label: "probe",
        status: "running",
        stdout: "started via ui\n",
      },
    },
  } as Record<string, any>;

  await page.route("**/v1/**", async (route, request) => {
    const url = new URL(request.url());
    const method = request.method();
    const path = url.pathname;

    if (method === "GET" && path === `/v1/agents/${agentId}/sessions`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, sessions: [{ session_id: sessionId, thread_id: "thread-ops" }] }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/sessions/${sessionId}`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          session: {
            session_id: sessionId,
            thread_id: "thread-ops",
            attachment: {
              client_id: "webui",
              lease_seconds: 120,
              lease_active: true,
            },
          },
        }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/sessions/${sessionId}/orchestration/status`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, counts: { workers: 2, services: 1, shells: 2 } }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/sessions/${sessionId}/orchestration/workers`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, workers: [{ id: "worker-1", kind: "service" }, { id: "worker-2", kind: "shell" }] }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/sessions/${sessionId}/orchestration/dependencies`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, dependencies: [{ capability: "@api.http", state: "satisfied" }] }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/sessions/${sessionId}/shells`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          shells: [
            { job_id: "bg-1", label: "build", status: "running" },
            { job_id: "bg-2", label: "probe", status: "running" },
          ],
        }),
      });
      return;
    }
    if (method === "POST" && path === `/v1/agents/${agentId}/sessions/${sessionId}/shells`) {
      const body = request.postDataJSON() as any;
      shellState["bg-2"] = {
        ok: true,
        job: {
          job_id: "bg-2",
          label: body?.label || "probe",
          status: "running",
          stdout: "started via ui\n",
        },
      };
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, job: shellState["bg-2"].job }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/sessions/${sessionId}/shells/bg-1`) {
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify(shellState["bg-1"]) });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/sessions/${sessionId}/shells/bg-2`) {
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify(shellState["bg-2"]) });
      return;
    }
    if (method === "POST" && path === `/v1/agents/${agentId}/sessions/${sessionId}/shells/bg-1/poll`) {
      shellState["bg-1"].job.stdout += "poll tick\n";
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify(shellState["bg-1"]) });
      return;
    }
    if (method === "POST" && path === `/v1/agents/${agentId}/sessions/${sessionId}/shells/bg-1/send`) {
      const body = request.postDataJSON() as any;
      shellState["bg-1"].job.stdout += `stdin:${body?.text || ""}\n`;
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify(shellState["bg-1"]) });
      return;
    }
    if (method === "POST" && path === `/v1/agents/${agentId}/sessions/${sessionId}/shells/bg-1/terminate`) {
      shellState["bg-1"].job.status = "done";
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify(shellState["bg-1"]) });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/sessions/${sessionId}/services`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, services: [{ job_id: "bg-7", label: "api", status: "ready" }] }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/sessions/${sessionId}/services/bg-7`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          service: {
            job_id: "bg-7",
            label: "api",
            status: "ready",
            endpoint: "http://127.0.0.1:9000",
            recipes: [{ name: "health" }],
          },
        }),
      });
      return;
    }
    if (method === "POST" && path === `/v1/agents/${agentId}/sessions/${sessionId}/services/bg-7/attach`) {
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true, attached: true }) });
      return;
    }
    if (method === "POST" && path === `/v1/agents/${agentId}/sessions/${sessionId}/services/bg-7/wait`) {
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true, ready: true }) });
      return;
    }
    if (method === "POST" && path === `/v1/agents/${agentId}/sessions/${sessionId}/services/bg-7/run`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, result: { status: "ok", body: "healthy" } }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/sessions/${sessionId}/capabilities`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, capabilities: [{ name: "@api.http", providers: ["bg-7"], consumers: ["bg-1"] }] }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/sessions/${sessionId}/capabilities/%40api.http`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, capability: { name: "@api.http", providers: ["bg-7"], consumers: ["bg-1"] } }),
      });
      return;
    }

    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/caps`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, features: { jobs: { enabled: true }, client_prefs: { enabled: false } } }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/health`) {
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true }) });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/config`) {
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true, config: {} }) });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/session/audit`) {
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true, entries: [] }) });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/session/client_events`) {
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true, events: [] }) });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/session/artifacts`) {
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true, artifacts: [] }) });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/session/scene`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, session_id: sessionId, updated_unix_ms: Date.now(), scene: {} }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/db/messages`) {
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true, messages: [] }) });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/db/runs`) {
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true, runs: [] }) });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/db/ui_actions`) {
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true, rows: [] }) });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/proxy/api/v1/db/client_events`) {
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true, rows: [] }) });
      return;
    }
    if (method === "GET" && path === `/v1/client_prefs`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, found: false, client_id: "webui", client_kind: "webui", version: 1, prefs: {} }),
      });
      return;
    }
    if (method === "POST" && path === `/v1/client_prefs`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, client_id: "webui", client_kind: "webui", version: 1, prefs: {} }),
      });
      return;
    }

    await route.abort();
  });

  await page.goto("/");
  await expect(page.getByTestId("settings-drawer")).toBeVisible();
  await page.getByTestId("session-id-input").fill(sessionId);
  await expect(page.getByTestId("broker-session-operators-section")).toBeVisible();
  await expect(page.getByTestId("broker-session-operators-section")).toContainText("Shell-first host examination");
  await expect(page.getByTestId("broker-session-operators-section")).toContainText('"workers": 2');

  const shellsSection = page.getByTestId("session-shells-section");
  const servicesSection = page.getByTestId("session-services-section");
  const capabilitiesSection = page.getByTestId("session-capabilities-section");

  await expect(shellsSection).toContainText("bg-1");
  await shellsSection.getByText("bg-1 · build · running").click();
  await expect(shellsSection).toContainText("initial shell output");

  await shellsSection.getByRole("button", { name: "Poll" }).click();
  await expect(shellsSection).toContainText("poll tick");

  await shellsSection.getByPlaceholder("stdin text").fill("echo hi");
  await shellsSection.getByRole("button", { name: "Send" }).click();
  await expect(shellsSection).toContainText("stdin:echo hi");

  await shellsSection.getByRole("button", { name: "Terminate" }).click();
  await expect(shellsSection).toContainText('"status": "done"');

  await page.getByTestId("session-shell-command-input").fill("printf shell-hello");
  await shellsSection.getByRole("button", { name: "Start shell" }).click();
  await expect(shellsSection).toContainText("bg-2");

  await expect(servicesSection).toContainText("bg-7 · api · ready");
  await servicesSection.getByRole("button", { name: "Attach" }).click();
  await expect(servicesSection).toContainText('"attached": true');
  await servicesSection.getByRole("button", { name: "Wait" }).click();
  await expect(servicesSection).toContainText('"ready": true');
  await servicesSection.getByRole("button", { name: "Run recipe" }).click();
  await expect(servicesSection).toContainText('"status": "ok"');

  await expect(capabilitiesSection).toContainText("@api.http");
  await capabilitiesSection.getByText("@api.http · providers=1 consumers=1").click();
  await expect(capabilitiesSection).toContainText('"providers": [');
  await expect(capabilitiesSection).toContainText('"bg-7"');
});
