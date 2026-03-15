import { expect, test } from "@playwright/test";
import { seedBrokerState } from "./brokerTestState";

test("broker audio panel manages signaling session lifecycle", async ({ page }) => {
  const agentId = "agent1";
  const sessionId = "aud_test_123";
  const now = Date.now();
  let sessions: any[] = [];
  const signalBodies: any[] = [];

  await seedBrokerState(page, { agentId });
  await page.addInitScript(() => {
    class FakeStream {
      id = "remote-stream-1";

      getTracks() {
        return [{ kind: "audio", id: "remote-audio-1" }];
      }
    }

    class FakePeerConnection extends EventTarget {
      connectionState = "new";
      signalingState = "stable";
      iceConnectionState = "new";
      localDescription: any = null;
      remoteDescription: any = null;
      onicecandidate: ((ev: any) => void) | null = null;
      ontrack: ((ev: any) => void) | null = null;
      onconnectionstatechange: (() => void) | null = null;
      onsignalingstatechange: (() => void) | null = null;
      oniceconnectionstatechange: (() => void) | null = null;

      addTransceiver() {
        return undefined;
      }

      async createOffer() {
        return { type: "offer", sdp: "fake-offer-sdp" };
      }

      async setLocalDescription(desc: any) {
        this.localDescription = desc;
        this.signalingState = "have-local-offer";
        this.onsignalingstatechange?.();
        setTimeout(() => {
          const payload = {
            candidate: "candidate:1 1 udp 2113937151 127.0.0.1 49999 typ host",
            sdpMid: "0",
            sdpMLineIndex: 0,
          };
          const ev: any = new Event("icecandidate");
          ev.candidate = {
            ...payload,
            toJSON() {
              return payload;
            },
          };
          this.onicecandidate?.(ev);
        }, 0);
      }

      async setRemoteDescription(desc: any) {
        this.remoteDescription = desc;
        this.signalingState = "stable";
        this.connectionState = "connected";
        this.iceConnectionState = "connected";
        this.onsignalingstatechange?.();
        this.onconnectionstatechange?.();
        this.oniceconnectionstatechange?.();
        setTimeout(() => {
          const ev: any = new Event("track");
          ev.track = { kind: "audio", id: "remote-audio-1" };
          ev.streams = [new FakeStream()];
          this.ontrack?.(ev);
        }, 0);
      }

      async addIceCandidate() {
        return undefined;
      }

      close() {
        this.connectionState = "closed";
        this.iceConnectionState = "closed";
        this.signalingState = "closed";
        this.onconnectionstatechange?.();
        this.oniceconnectionstatechange?.();
        this.onsignalingstatechange?.();
      }
    }

    (window as any).__agentuiRtcFactory = () => new FakePeerConnection();
  });

  await page.route("**/v1/**", async (route, request) => {
    const url = new URL(request.url());
    const path = url.pathname;
    const method = request.method();

    if (method === "GET" && path === "/v1/agents") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          agents: [
            {
              agent_id: agentId,
              enabled: true,
              created_unix_ms: now,
              owner_sub: "owner",
              connected: true,
              deployments: [{ deployment_id: "default", connected: true }],
            },
          ],
        }),
      });
      return;
    }
    if (method === "GET" && path === "/v1/connectors") {
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true, count: 0, connectors: [] }) });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/members`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, agent_id: agentId, owner_sub: "owner", members: [] }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/membership_audit`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, agent_id: agentId, owner_sub: "owner", audit: [] }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/agents/${agentId}/deployments`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          agent_id: agentId,
          default_deployment_id: "default",
          deployments: [{ deployment_id: "default", connected: true }],
        }),
      });
      return;
    }
    if (method === "GET" && path === "/v1/events/replay") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, events: [], count: 0, next_since_ts: now }),
      });
      return;
    }
    if (method === "GET" && path === "/v1/events") {
      await route.fulfill({ status: 200, contentType: "text/event-stream", body: "" });
      return;
    }
    if (method === "GET" && path === "/v1/client_prefs") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, found: false, client_id: "webui", client_kind: "webui", version: 1, prefs: {} }),
      });
      return;
    }
    if (method === "POST" && path === "/v1/client_prefs") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, client_id: "webui", client_kind: "webui", version: 1, prefs: {} }),
      });
      return;
    }
    if (method === "GET" && path === "/v1/teams") {
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true, teams: [] }) });
      return;
    }
    if (method === "GET" && path === "/v1/audio/sessions") {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, count: sessions.length, sessions }),
      });
      return;
    }
    if (method === "POST" && path === "/v1/audio/sessions") {
      sessions = [
        {
          session_id: sessionId,
          agent_id: agentId,
          deployment_id: "default",
          owner_sub: "owner",
          mode: "webrtc",
          created_unix_ms: now,
          expires_unix_ms: now + 15 * 60 * 1000,
          subscriber_count: 1,
          signal_count: 0,
        },
      ];
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({
          ok: true,
          session_id: sessionId,
          expires_unix_ms: now + 15 * 60 * 1000,
          signal: {
            send_url: `/v1/audio/sessions/${sessionId}/signal`,
            recv_url: `/v1/audio/sessions/${sessionId}/signal/stream`,
          },
        }),
      });
      return;
    }
    if (method === "GET" && path === `/v1/audio/sessions/${sessionId}`) {
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, session: sessions[0] }),
      });
      return;
    }
    if (method === "POST" && path === `/v1/audio/sessions/${sessionId}/signal`) {
      const signalBody = request.postDataJSON();
      signalBodies.push(signalBody);
      sessions = sessions.map((session) => ({
        ...session,
        signal_count: 1,
        last_signal_type: String(signalBody?.type || ""),
        last_signal_from: "webui",
        last_signal_unix_ms: now + 1,
      }));
      await route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ok: true }) });
      return;
    }
    if (method === "GET" && path === `/v1/audio/sessions/${sessionId}/signal/stream`) {
      await route.fulfill({
        status: 200,
        contentType: "text/event-stream",
        body:
          ":ok\n\n" +
          `event: signal\ndata: ${JSON.stringify({ type: "answer", from: "agentd", ts_unix_ms: now + 2, payload: { type: "answer", sdp: "fake-answer-sdp" } })}\n\n` +
          `event: signal\ndata: ${JSON.stringify({ type: "candidate", from: "agentd", ts_unix_ms: now + 3, payload: { candidate: "candidate:2 1 udp 2113937151 127.0.0.1 50000 typ host", sdpMid: "0", sdpMLineIndex: 0 } })}\n\n`,
      });
      return;
    }
    if (method === "DELETE" && path === `/v1/audio/sessions/${sessionId}`) {
      sessions = [];
      await route.fulfill({
        status: 200,
        contentType: "application/json",
        body: JSON.stringify({ ok: true, deleted: true, session_id: sessionId }),
      });
      return;
    }

    await route.fallback();
  });

  await page.goto("/");
  await page.getByRole("button", { name: "Broker Console" }).click();
  const brokerPanel = page.locator("details").filter({ has: page.getByText("Broker panel", { exact: true }) });
  await brokerPanel.getByRole("button", { name: "Audio" }).click();

  await expect(page.getByTestId("broker-audio-section")).toBeVisible();
  await page.getByTestId("broker-audio-create").click();

  await expect(page.getByTestId(`broker-audio-session-${sessionId}`)).toBeVisible();
  await expect(page.getByTestId("broker-audio-selected-session")).toContainText(sessionId);
  await expect(page.getByTestId("broker-audio-signal-events")).toContainText("candidate");
  await expect(page.getByTestId("broker-audio-signal-events")).toContainText("answer");

  await page.getByTestId("broker-audio-webrtc-connect").click();
  await expect.poll(() => signalBodies.map((row) => row?.type || "")).toContain("offer");
  await expect(page.getByTestId("broker-audio-webrtc-status")).toContainText("state: connected");
  await expect(page.getByTestId("broker-audio-webrtc-status")).toContainText("remote tracks: 1");
  await expect(page.getByTestId("broker-audio-webrtc-status")).toContainText("last remote signal: candidate");

  await page.getByTestId("broker-audio-signal-type").selectOption("control");
  await page.getByTestId("broker-audio-signal-payload").fill('{"state":"ready"}');
  await page.getByTestId("broker-audio-send").click();

  await expect.poll(() => signalBodies.at(-1)?.type ?? "").toBe("control");
  await expect(page.getByTestId("broker-audio-selected-session")).toContainText("last signal: control from webui");

  await page.getByTestId("broker-audio-webrtc-disconnect").click();
  await expect.poll(() => signalBodies.map((row) => row?.type || "")).toContain("bye");

  await page.getByTestId("broker-audio-delete").click();
  await expect(page.getByText("No live audio sessions for the current filter.")).toBeVisible();
});
