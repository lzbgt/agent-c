#!/usr/bin/env node
"use strict";

const path = require("path");

const [, , brokerUrl, sessionId, token, senderTagArg] = process.argv;
const senderTag = senderTagArg || "webui_playwright_peer";

if (!brokerUrl || !sessionId || !token) {
  process.stderr.write(
    "usage: agentd_voice_webrtc_peer_receiver.js <broker-url> <session-id> <token> [sender-tag]\n"
  );
  process.exit(2);
}
if (!process.env.ROOT_PATH) {
  process.stderr.write("ROOT_PATH is required\n");
  process.exit(2);
}

const { chromium } = require(path.resolve(process.env.ROOT_PATH, "ui/node_modules/playwright"));

function join(base, suffix) {
  return base.replace(/\/+$/, "") + (suffix.startsWith("/") ? suffix : "/" + suffix);
}

async function sendSignal(type, payload) {
  const bodyPayload = payload && typeof payload === "object" ? { ...payload, sender_tag: senderTag } : { sender_tag: senderTag };
  const res = await fetch(join(brokerUrl, `/v1/audio/sessions/${encodeURIComponent(sessionId)}/signal`), {
    method: "POST",
    headers: {
      Authorization: `Bearer ${token}`,
      "Content-Type": "application/json",
    },
    body: JSON.stringify({ type, payload: bodyPayload }),
  });
  if (!res.ok) throw new Error(`send ${type} failed: http ${res.status} ${await res.text()}`);
}

async function run() {
  const browser = await chromium.launch({ headless: true, args: ["--autoplay-policy=no-user-gesture-required"] });
  const page = await browser.newPage();
  await page.exposeFunction("__webuiSendSignal", async (type, payload) => {
    await sendSignal(type, payload || {});
  });
  await page.setContent(`<!doctype html><html><body><script>
  (() => {
    const state = {
      connectionState: "new",
      signalingState: "stable",
      iceConnectionState: "new",
      remoteTrackCount: 0,
      sentCandidateCount: 0,
      receivedCandidateCount: 0,
      inboundPacketsReceived: 0,
      inboundBytesReceived: 0,
      audioReady: false,
    };
    let peer = null;
    let remoteAudio = null;
    let remoteStream = null;
    let pendingRemoteCandidates = [];
    function candidatePayload(candidate) {
      if (!candidate) return null;
      if (typeof candidate.toJSON === "function") return candidate.toJSON();
      return {
        candidate: candidate.candidate,
        sdpMid: candidate.sdpMid ?? undefined,
        sdpMLineIndex: candidate.sdpMLineIndex ?? undefined,
        usernameFragment: candidate.usernameFragment ?? undefined,
      };
    }
    async function ensurePeer() {
      if (peer) return peer;
      peer = new RTCPeerConnection({ iceServers: [] });
      peer.addTransceiver("audio", { direction: "recvonly" });
      remoteAudio = document.createElement("audio");
      remoteAudio.autoplay = true;
      remoteAudio.controls = true;
      document.body.appendChild(remoteAudio);
      peer.onicecandidate = async (event) => {
        if (!event.candidate) return;
        state.sentCandidateCount += 1;
        await window.__webuiSendSignal("candidate", candidatePayload(event.candidate));
      };
      peer.ontrack = (event) => {
        state.remoteTrackCount += 1;
        remoteStream = event.streams && event.streams[0] ? event.streams[0] : new MediaStream([event.track]);
        remoteAudio.srcObject = remoteStream;
        state.audioReady = !!remoteAudio.srcObject;
      };
      peer.onconnectionstatechange = () => { state.connectionState = peer.connectionState || "new"; };
      peer.onsignalingstatechange = () => { state.signalingState = peer.signalingState || "stable"; };
      peer.oniceconnectionstatechange = () => { state.iceConnectionState = peer.iceConnectionState || "new"; };
      return peer;
    }
    async function start() {
      const pc = await ensurePeer();
      const offer = await pc.createOffer();
      await pc.setLocalDescription(offer);
      await window.__webuiSendSignal("offer", { type: offer.type, sdp: offer.sdp });
    }
    async function handleSignal(ev) {
      const pc = await ensurePeer();
      if (!ev || typeof ev !== "object") return;
      if (ev.type === "answer") {
        const payload = ev.payload || {};
        if (!payload.sdp) return;
        await pc.setRemoteDescription({ type: payload.type || "answer", sdp: payload.sdp });
        for (const candidate of pendingRemoteCandidates) await pc.addIceCandidate(candidate);
        pendingRemoteCandidates = [];
        return;
      }
      if (ev.type === "candidate") {
        const payload = ev.payload || {};
        if (!payload.candidate) return;
        if (!pc.remoteDescription) pendingRemoteCandidates.push(payload);
        else await pc.addIceCandidate(payload);
        state.receivedCandidateCount += 1;
        return;
      }
      if (ev.type === "bye") {
        pc.close();
      }
    }
    async function getState() {
      if (peer) {
        const stats = await peer.getStats();
        for (const report of stats.values()) {
          const mediaType = report.mediaType || report.kind || "";
          if (report.type === "inbound-rtp" && mediaType === "audio") {
            state.inboundPacketsReceived = Math.max(state.inboundPacketsReceived, report.packetsReceived || 0);
            state.inboundBytesReceived = Math.max(state.inboundBytesReceived, report.bytesReceived || 0);
          }
        }
      }
      return { ...state };
    }
    window.__webuiAudioStart = start;
    window.__webuiAudioHandleSignal = handleSignal;
    window.__webuiAudioGetState = getState;
  })();
  </script></body></html>`);

  const streamAbort = new AbortController();
  const streamPromise = (async () => {
    const res = await fetch(join(brokerUrl, `/v1/audio/sessions/${encodeURIComponent(sessionId)}/signal/stream`), {
      method: "GET",
      headers: { Authorization: `Bearer ${token}`, Accept: "text/event-stream" },
      signal: streamAbort.signal,
    });
    if (!res.ok || !res.body) throw new Error(`signal stream failed: http ${res.status}`);
    const reader = res.body.getReader();
    const decoder = new TextDecoder();
    let buffer = "";
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      buffer += decoder.decode(value, { stream: true });
      for (;;) {
        const idx = buffer.indexOf("\n");
        if (idx < 0) break;
        let line = buffer.slice(0, idx);
        buffer = buffer.slice(idx + 1);
        if (line.endsWith("\r")) line = line.slice(0, -1);
        if (!line.startsWith("data:")) continue;
        const raw = line.slice(5).trim();
        if (!raw) continue;
        let ev;
        try { ev = JSON.parse(raw); } catch { continue; }
        const payload = ev && ev.payload && typeof ev.payload === "object" ? ev.payload : {};
        if (String(payload.sender_tag || "").trim() === senderTag) continue;
        await page.evaluate((msg) => window.__webuiAudioHandleSignal(msg), ev);
      }
    }
  })().catch((err) => {
    if (err && (err.name === "AbortError" || String(err).includes("AbortError"))) return;
    throw err;
  });

  await page.evaluate(() => window.__webuiAudioStart());

  const deadline = Date.now() + 15000;
  let finalState = null;
  while (Date.now() < deadline) {
    const state = await page.evaluate(() => window.__webuiAudioGetState());
    finalState = state;
    if (
      state.connectionState === "connected" &&
      state.iceConnectionState === "connected" &&
      state.remoteTrackCount >= 1 &&
      state.inboundPacketsReceived > 0 &&
      state.inboundBytesReceived > 0
    ) {
      break;
    }
    await new Promise((resolve) => setTimeout(resolve, 150));
  }

  if (
    !finalState ||
    finalState.connectionState !== "connected" ||
    finalState.iceConnectionState !== "connected" ||
    finalState.remoteTrackCount < 1 ||
    finalState.inboundPacketsReceived <= 0 ||
    finalState.inboundBytesReceived <= 0
  ) {
    throw new Error("webrtc media did not reach connected inbound-rtp state: " + JSON.stringify(finalState));
  }

  streamAbort.abort();
  await browser.close();
  await streamPromise.catch(() => {});
  process.stdout.write(JSON.stringify({ ok: true, state: finalState }) + "\n");
}

run().catch((err) => {
  process.stderr.write(String(err) + "\n");
  process.exit(1);
});
