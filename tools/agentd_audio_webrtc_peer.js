#!/usr/bin/env node

const path = require("path");
const fs = require("fs");

function loadPlaywright() {
  const candidates = [
    path.resolve(__dirname, "../ui/node_modules/playwright"),
    "playwright",
  ];
  for (const candidate of candidates) {
    try {
      return require(candidate);
    } catch (_) {
      // try next
    }
  }
  throw new Error("playwright module not found");
}

const { chromium } = loadPlaywright();

function trimCopy(value) {
  return String(value ?? "").trim();
}

function joinBasePath(base, suffix) {
  let out = trimCopy(base);
  while (out.endsWith("/")) out = out.slice(0, -1);
  if (!suffix) return out;
  if (suffix.startsWith("/")) return out + suffix;
  return out + "/" + suffix;
}

function parseArgs(argv) {
  const out = {
    brokerUrl: "",
    token: "",
    sessionId: "",
    deadlineMs: 15000,
    pollIntervalMs: 100,
    toneHz: 440,
    readyFile: "",
    senderTag: "agentd_playwright_peer",
    verbose: false,
  };
  for (let i = 2; i < argv.length; i += 1) {
    const a = argv[i];
    if (a === "--broker-url" && i + 1 < argv.length) out.brokerUrl = argv[++i];
    else if (a === "--token" && i + 1 < argv.length) out.token = argv[++i];
    else if (a === "--session-id" && i + 1 < argv.length) out.sessionId = argv[++i];
    else if (a === "--deadline-ms" && i + 1 < argv.length) out.deadlineMs = Number(argv[++i]);
    else if (a === "--poll-interval-ms" && i + 1 < argv.length) out.pollIntervalMs = Number(argv[++i]);
    else if (a === "--tone-hz" && i + 1 < argv.length) out.toneHz = Number(argv[++i]);
    else if (a === "--ready-file" && i + 1 < argv.length) out.readyFile = argv[++i];
    else if (a === "--sender-tag" && i + 1 < argv.length) out.senderTag = argv[++i];
    else if (a === "--verbose") out.verbose = true;
    else if (a === "--help" || a === "-h") out.help = true;
    else throw new Error(`unknown arg: ${a}`);
  }
  return out;
}

function usage() {
  return (
    "Usage: agentd_audio_webrtc_peer.js" +
    " --broker-url <url> --token <token> --session-id <id>" +
    " [--deadline-ms <ms>] [--poll-interval-ms <ms>] [--tone-hz <hz>] [--ready-file <path>] [--sender-tag <tag>] [--verbose]"
  );
}

async function sendSignal(opt, type, payload) {
  const bodyPayload = payload && typeof payload === "object" ? { ...payload, sender_tag: opt.senderTag } : { sender_tag: opt.senderTag };
  const res = await fetch(joinBasePath(opt.brokerUrl, `/v1/audio/sessions/${encodeURIComponent(opt.sessionId)}/signal`), {
    method: "POST",
    headers: {
      Authorization: `Bearer ${opt.token}`,
      "Content-Type": "application/json",
    },
    body: JSON.stringify({ type, payload: bodyPayload }),
  });
  if (!res.ok) {
    throw new Error(`send signal ${type} failed: http ${res.status} ${await res.text()}`);
  }
}

async function consumeSignalStream(opt, onEvent, onOpen) {
  const res = await fetch(
    joinBasePath(opt.brokerUrl, `/v1/audio/sessions/${encodeURIComponent(opt.sessionId)}/signal/stream`),
    {
      method: "GET",
      headers: {
        Authorization: `Bearer ${opt.token}`,
        Accept: "text/event-stream",
      },
    },
  );
  if (!res.ok || !res.body) {
    throw new Error(`signal stream failed: http ${res.status}`);
  }
  if (typeof onOpen === "function") await onOpen();
  const reader = res.body.getReader();
  const decoder = new TextDecoder();
  let buffer = "";
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    buffer += decoder.decode(value, { stream: true });
    for (;;) {
      const split = buffer.indexOf("\n");
      if (split < 0) break;
      let line = buffer.slice(0, split);
      buffer = buffer.slice(split + 1);
      if (line.endsWith("\r")) line = line.slice(0, -1);
      if (!line.startsWith("data:")) continue;
      const raw = trimCopy(line.slice(5));
      if (!raw) continue;
      let parsed;
      try {
        parsed = JSON.parse(raw);
      } catch (_) {
        continue;
      }
      await onEvent(parsed);
    }
  }
}

async function main() {
  const opt = parseArgs(process.argv);
  if (opt.help) {
    console.error(usage());
    process.exit(2);
  }
  if (!opt.brokerUrl || !opt.token || !opt.sessionId) {
    console.error(usage());
    process.exit(2);
  }

  let browser;
  let page;
  let closedByRemote = false;
  let byeSent = false;
  let shuttingDown = false;
  const deadlineAt = Date.now() + opt.deadlineMs;
  let streamOpened = false;

  async function sendByeOnce(reason) {
    if (byeSent) return;
    byeSent = true;
    try {
      await sendSignal(opt, "bye", { reason: reason || "agentd_peer_shutdown" });
    } catch (_) {
      // best effort during shutdown
    }
  }

  async function gracefulSignalShutdown(reason) {
    if (shuttingDown) return;
    shuttingDown = true;
    try {
      await sendByeOnce(reason);
      let state = {};
      if (page) {
        try {
          state = await page.evaluate(() => (window.__agentdAudioGetState ? window.__agentdAudioGetState() : {}));
        } catch (_) {
          state = {};
        }
      }
      process.stdout.write(`${JSON.stringify({ ok: true, stopped: true, reason, state })}\n`);
    } finally {
      if (browser) await browser.close().catch(() => {});
      process.exit(0);
    }
  }

  process.on("SIGTERM", () => { void gracefulSignalShutdown("sigterm"); });
  process.on("SIGINT", () => { void gracefulSignalShutdown("sigint"); });

  try {
    browser = await chromium.launch({
      headless: true,
      args: ["--autoplay-policy=no-user-gesture-required"],
    });
    page = await browser.newPage();
    await page.exposeFunction("__agentdAudioSendSignal", async (type, payload) => {
      await sendSignal(opt, type, payload || {});
    });
    await page.setContent(
      `<!doctype html><html><body><script>
      (() => {
        const toneHz = ${JSON.stringify(opt.toneHz)};
        const state = {
          connectionState: "new",
          signalingState: "stable",
          iceConnectionState: "new",
          remoteTrackCount: 0,
          answerSent: false,
          sentCandidateCount: 0,
          receivedCandidateCount: 0,
          remoteOfferSeen: false,
          closedByRemote: false,
          error: "",
        };
        let ctx = null;
        let osc = null;
        let gain = null;
        let dest = null;
        let peer = null;
        let pendingRemoteCandidates = [];
        function isRelayIceCandidate(value) {
          return typeof value === "string" && /(?:^|\\s)typ\\s+relay(?:\\s|$)/i.test(value);
        }
        function stripRelayIceCandidatesFromSdp(sdp) {
          if (!sdp) return sdp;
          const hadTrailingNewline = /\\r?\\n$/.test(sdp);
          const lines = String(sdp).split(/\\r?\\n/);
          if (hadTrailingNewline) lines.pop();
          const kept = lines.filter((line) => {
            const trimmed = line.trim();
            const isCandidate = trimmed.startsWith("a=candidate:") || trimmed.startsWith("candidate:");
            return !isCandidate || !isRelayIceCandidate(trimmed);
          });
          return kept.join("\\r\\n") + (hadTrailingNewline ? "\\r\\n" : "");
        }
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
          ctx = new (window.AudioContext || window.webkitAudioContext)();
          dest = ctx.createMediaStreamDestination();
          gain = ctx.createGain();
          gain.gain.value = 0.03;
          osc = ctx.createOscillator();
          osc.type = "sine";
          osc.frequency.value = Number(toneHz) || 440;
          osc.connect(gain);
          gain.connect(dest);
          osc.start();
          if (typeof ctx.resume === "function") await ctx.resume();
          peer = new RTCPeerConnection({ iceServers: [] });
          for (const track of dest.stream.getTracks()) peer.addTrack(track, dest.stream);
          peer.onconnectionstatechange = () => { state.connectionState = peer.connectionState || "new"; };
          peer.onsignalingstatechange = () => { state.signalingState = peer.signalingState || "stable"; };
          peer.oniceconnectionstatechange = () => { state.iceConnectionState = peer.iceConnectionState || "new"; };
          peer.onicecandidate = async (event) => {
            if (!event.candidate) return;
            const payload = candidatePayload(event.candidate);
            if (!payload || isRelayIceCandidate(payload.candidate)) return;
            state.sentCandidateCount += 1;
            await window.__agentdAudioSendSignal("candidate", payload);
          };
          peer.ontrack = () => {
            state.remoteTrackCount += 1;
          };
          return peer;
        }
        async function handleSignal(ev) {
          const pc = await ensurePeer();
          if (!ev || typeof ev !== "object") return;
          if (ev.type === "offer") {
            const payload = ev.payload || {};
            if (!payload.sdp) return;
            state.remoteOfferSeen = true;
            await pc.setRemoteDescription({ type: payload.type || "offer", sdp: stripRelayIceCandidatesFromSdp(payload.sdp) });
            for (const candidate of pendingRemoteCandidates) await pc.addIceCandidate(candidate);
            pendingRemoteCandidates = [];
            const answer = await pc.createAnswer();
            await pc.setLocalDescription(answer);
            await window.__agentdAudioSendSignal("answer", {
              type: answer.type,
              sdp: stripRelayIceCandidatesFromSdp(answer.sdp || ""),
            });
            state.answerSent = true;
            return;
          }
          if (ev.type === "candidate") {
            const payload = ev.payload || {};
            if (!payload.candidate) return;
            if (isRelayIceCandidate(payload.candidate)) return;
            if (!pc.remoteDescription) {
              pendingRemoteCandidates.push(payload);
            } else {
              await pc.addIceCandidate(payload);
            }
            state.receivedCandidateCount += 1;
            return;
          }
          if (ev.type === "bye") {
            state.closedByRemote = true;
            if (peer) peer.close();
          }
        }
        async function getState() {
          return { ...state };
        }
        window.__agentdAudioHandleSignal = handleSignal;
        window.__agentdAudioGetState = getState;
      })();
      </script></body></html>`,
    );

    const streamPromise = consumeSignalStream(opt, async (ev) => {
      const payload = ev && ev.payload && typeof ev.payload === "object" ? ev.payload : {};
      if (trimCopy(payload.sender_tag) === opt.senderTag) return;
      const from = trimCopy((ev && ev.from) || "").toLowerCase();
      if (opt.verbose) process.stderr.write(`[agentd-audio-peer] recv ${ev.type} from ${from || "unknown"}\n`);
      await page.evaluate((msg) => window.__agentdAudioHandleSignal(msg), ev);
      if (ev.type === "bye") closedByRemote = true;
    }, async () => {
      streamOpened = true;
      if (opt.readyFile) {
        fs.writeFileSync(opt.readyFile, JSON.stringify({ ok: true, session_id: opt.sessionId }) + "\n");
      }
    });

    for (;;) {
      if (shuttingDown) {
        await new Promise((resolve) => setTimeout(resolve, 25));
        continue;
      }
      if (!streamOpened && Date.now() >= deadlineAt) {
        process.stdout.write(`${JSON.stringify({ ok: false, error: "signal stream did not open" })}\n`);
        process.exitCode = 1;
        break;
      }
      const state = await page.evaluate(() => window.__agentdAudioGetState());
      if (opt.verbose) {
        process.stderr.write(
          `[agentd-audio-peer] state conn=${state.connectionState} ice=${state.iceConnectionState} sig=${state.signalingState} remoteOffer=${state.remoteOfferSeen} answer=${state.answerSent}\n`,
        );
      }
      if (closedByRemote || state.closedByRemote) {
        process.stdout.write(`${JSON.stringify({ ok: true, closed_by_remote: true, state })}\n`);
        break;
      }
      if (Date.now() >= deadlineAt) {
        process.stdout.write(`${JSON.stringify({ ok: false, error: "deadline exceeded", state })}\n`);
        process.exitCode = 1;
        break;
      }
      await new Promise((resolve) => setTimeout(resolve, opt.pollIntervalMs));
    }
    await browser.close();
    await streamPromise.catch((err) => {
      if (opt.verbose) process.stderr.write(`[agentd-audio-peer] stream end: ${String(err)}\n`);
    });
  } catch (err) {
    if (shuttingDown) {
      if (browser) await browser.close().catch(() => {});
      process.exitCode = 0;
      return;
    }
    if (page) {
      try {
        const state = await page.evaluate(() => (window.__agentdAudioGetState ? window.__agentdAudioGetState() : {}));
        process.stdout.write(`${JSON.stringify({ ok: false, error: String(err), state })}\n`);
      } catch (_) {
        process.stdout.write(`${JSON.stringify({ ok: false, error: String(err) })}\n`);
      }
    } else {
      process.stdout.write(`${JSON.stringify({ ok: false, error: String(err) })}\n`);
    }
    process.exitCode = 1;
    if (browser) await browser.close().catch(() => {});
  }
}

main().catch((err) => {
  process.stdout.write(`${JSON.stringify({ ok: false, error: String(err) })}\n`);
  process.exit(1);
});
