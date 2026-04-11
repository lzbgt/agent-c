#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
BUILTIN_MODE="${2:-signaling_stub}"
BUILTIN_NATIVE_LIBRARY="${3:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "usage: $0 <agentd> [signaling_stub|native_plugin] [builtin_native_library]" >&2
  exit 2
fi
if [[ ! -x "${AGENTD_BIN}" ]]; then
  echo "SKIP: agentd binary not executable" >&2
  exit 77
fi
if ! command -v go >/dev/null 2>&1; then
  echo "SKIP: go not found" >&2
  exit 77
fi
if ! command -v python3 >/dev/null 2>&1; then
  echo "SKIP: python3 not found" >&2
  exit 77
fi

PG_LIB="${SCRIPT_DIR}/lib/pg_test_lib.sh"
if [[ -f "${PG_LIB}" ]]; then
  # shellcheck disable=SC1090
  source "${PG_LIB}"
fi

ROOT="$(agentd_smoke_project_root)"
MODE_TAG="$(printf '%s' "${BUILTIN_MODE}" | tr -cs '[:alnum:]' '_' | sed 's/^_//;s/_$//')"
LOG_DIR="${ROOT}/build"
mkdir -p "${LOG_DIR}"
LOG_FILE="${LOG_DIR}/agentd_session_voice_webrtc_peer_builtin_smoke_${MODE_TAG}.log"

PG_DSN_OVERRIDE="${AGENTD_TEST_PG_DSN:-}"
USE_DOCKER="1"
USE_LOCAL_PG="0"
if [[ -n "${PG_DSN_OVERRIDE}" ]]; then
  USE_DOCKER="0"
elif ! command -v docker >/dev/null 2>&1; then
  USE_DOCKER="0"
elif ! timeout 2 docker info >/dev/null 2>&1; then
  USE_DOCKER="0"
fi

if [[ "${USE_DOCKER}" == "0" && -z "${PG_DSN_OVERRIDE}" ]]; then
  if pg_test_has_local_pg; then
    USE_LOCAL_PG="1"
  else
    reason="$(pg_test_unavailable_reason)"
    echo "SKIP: docker not ready and local Postgres not available (${reason}); set AGENTD_TEST_PG_DSN to run" >&2
    exit 77
  fi
fi

POSTGRES_NAME="agentd_voice_builtin_smoke_${MODE_TAG}"
BROKER_PID=""
PG_PORT=""
BROKER_PORT=""
DAEMON_PORT=""

pick_port() {
  python3 - <<'PY'
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

cleanup() {
  agentd_smoke_stop || true
  if [[ -n "${BROKER_PID}" ]]; then
    kill -TERM "${BROKER_PID}" >/dev/null 2>&1 || true
    wait "${BROKER_PID}" >/dev/null 2>&1 || true
  fi
  if [[ "${USE_DOCKER}" == "1" ]]; then
    docker rm -f "${POSTGRES_NAME}" >/dev/null 2>&1 || true
  fi
  if [[ "${USE_LOCAL_PG}" == "1" ]]; then
    pg_test_stop_local || true
  fi
}
trap cleanup EXIT

if [[ "${USE_DOCKER}" == "1" ]]; then
  PG_PORT="$(pick_port)"
  if docker ps -a --format '{{.Names}}' | grep -q "^${POSTGRES_NAME}$"; then
    docker rm -f "${POSTGRES_NAME}" >/dev/null 2>&1 || true
  fi
  docker run -d --rm --name "${POSTGRES_NAME}" -e POSTGRES_PASSWORD=postgres -p "${PG_PORT}:5432" postgres:16 >/dev/null
  for _ in $(seq 1 30); do
    if docker exec "${POSTGRES_NAME}" pg_isready -U postgres >/dev/null 2>&1; then
      break
    fi
    sleep 1
  done
  if ! docker exec "${POSTGRES_NAME}" pg_isready -U postgres >/dev/null 2>&1; then
    echo "Postgres did not become ready" >&2
    exit 1
  fi
fi

if [[ "${USE_LOCAL_PG}" == "1" ]]; then
  if ! pg_test_start_local; then
    echo "SKIP: local Postgres init failed" >&2
    exit 77
  fi
fi

BROKER_PORT="$(pick_port)"
DAEMON_PORT="$(pick_port)"
DSN="${PG_DSN_OVERRIDE}"
if [[ -z "${DSN}" ]]; then
  if [[ "${USE_LOCAL_PG}" == "1" ]]; then
    DSN="${PG_TEST_DSN}"
  else
    DSN="postgres://postgres:postgres@127.0.0.1:${PG_PORT}/postgres?sslmode=disable"
  fi
fi

CLIENT_AUTH_JSON="${LOG_DIR}/broker_audio_builtin_client_auth.json"
cat >"${CLIENT_AUTH_JSON}" <<JSON
{
  "clients": [
    { "client_id": "audio-agentd", "token": "audio-agentd-token", "admin": true },
    { "client_id": "audio-webui", "token": "audio-webui-token", "admin": true }
  ]
}
JSON

BROKER_BIN="${LOG_DIR}/agentd-broker-audio-builtin"
(
  cd "${ROOT}/broker"
  go build -trimpath -o "${BROKER_BIN}" ./cmd/agentd-broker
) >>"${LOG_FILE}" 2>&1

"${BROKER_BIN}" \
  --listen "127.0.0.1:${BROKER_PORT}" \
  --db-dsn "${DSN}" \
  --client-auth-file "${CLIENT_AUTH_JSON}" >>"${LOG_FILE}" 2>&1 &
BROKER_PID=$!

for _ in $(seq 1 60); do
  if curl -fsS --noproxy "*" "http://127.0.0.1:${BROKER_PORT}/healthz" >/dev/null 2>&1; then
    break
  fi
  sleep 0.2
done
if ! curl -fsS --noproxy "*" "http://127.0.0.1:${BROKER_PORT}/healthz" >/dev/null 2>&1; then
  echo "Broker did not become ready; see ${LOG_FILE}" >&2
  exit 1
fi

DAEMON_TOKEN="agentd-builtin-smoke-token"
export AGENTD_AUTH_TOKEN="${DAEMON_TOKEN}"
export AGENTD_AUDIO_WEBRTC_BUILTIN_MODE="${BUILTIN_MODE}"
export AGENTD_AUDIO_WEBRTC_BROKER_URL="http://127.0.0.1:${BROKER_PORT}"
export AGENTD_AUDIO_WEBRTC_BROKER_TOKEN="audio-agentd-token"
if [[ "${BUILTIN_MODE}" == "native_plugin" ]]; then
  if [[ -z "${BUILTIN_NATIVE_LIBRARY}" || ! -f "${BUILTIN_NATIVE_LIBRARY}" ]]; then
    echo "SKIP: builtin native media engine library missing for native_plugin mode" >&2
    exit 77
  fi
  export AGENTD_AUDIO_WEBRTC_BUILTIN_NATIVE_LIBRARY="${BUILTIN_NATIVE_LIBRARY}"
fi
agentd_smoke_start "${AGENTD_BIN}" "127.0.0.1" "${DAEMON_PORT}" "agentd_session_voice_webrtc_peer_builtin_smoke" >>"${LOG_FILE}" 2>&1
unset AGENTD_AUTH_TOKEN
unset AGENTD_AUDIO_WEBRTC_BUILTIN_MODE
unset AGENTD_AUDIO_WEBRTC_BROKER_URL
unset AGENTD_AUDIO_WEBRTC_BROKER_TOKEN
unset AGENTD_AUDIO_WEBRTC_BUILTIN_NATIVE_LIBRARY

DAEMON_URL="http://127.0.0.1:${DAEMON_PORT}"
for _ in $(seq 1 100); do
  if curl -fsS --noproxy "*" --max-time 10 \
    -H "Authorization: Bearer ${DAEMON_TOKEN}" \
    "${DAEMON_URL}/api/v1/health" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

SESSION_ID="agentd_voice_builtin_smoke_$(date +%s)_$RANDOM"
curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H "Content-Type: application/json" \
  -d "{\"session_id\":\"${SESSION_ID}\"}" \
  "${DAEMON_URL}/api/v1/session/new" >/dev/null

START_JSON="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H "Content-Type: application/json" \
  -d "{\"session_id\":\"${SESSION_ID}\",\"action\":\"start\",\"runtime_kind\":\"builtin\",\"broker_agent_id\":\"a-1\",\"broker_deployment_id\":\"builtin-smoke\"}" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"

BROKER_SESSION_ID="$(python3 - "${START_JSON}" "${BUILTIN_MODE}" <<'PY'
import json, sys
obj = json.loads(sys.argv[1])
mode = sys.argv[2]
assert obj.get("ok") is True, obj
assert obj.get("builtin_available") is True, obj
peer = obj.get("peer") or {}
assert peer.get("runtime_kind") == "builtin", obj
expected_kind = "builtin_signaling_stub" if mode == "signaling_stub" else "builtin_native_plugin"
assert peer.get("media_engine_kind") == expected_kind, obj
assert peer.get("media_engine_state") == "signaling_ready", obj
assert peer.get("media_events_total") == 2, obj
assert peer.get("media_answers_sent") == 0, obj
assert peer.get("media_remote_offers_seen") == 0, obj
assert peer.get("running") is True and peer.get("ready") is True, obj
assert peer.get("managed_broker_session") is True, obj
assert obj.get("builtin_start_contract", {}).get("mutating_broker_actions_deferred") is False, obj
if mode == "native_plugin":
    probe = obj.get("builtin_native_probe") or {}
    provider = (peer.get("native_media_provider") or {})
    provider_name = (provider.get("name") or "")
    provider_caps = provider.get("capabilities") or {}
    expected_native_supported = provider_name == "agentd_builtin_embedded_transport_provider"
    assert probe.get("loadable") is True, obj
    assert probe.get("native_media_supported") is expected_native_supported, obj
    assert (probe.get("provider") or {}).get("abi_version") == 5, obj
    assert (probe.get("provider") or {}).get("name") in {
        "agentd_builtin_sample_provider",
        "agentd_builtin_embedded_transport_provider",
    }, obj
    assert provider.get("abi_version") == 5, obj
    assert provider_name in {
        "agentd_builtin_sample_provider",
        "agentd_builtin_embedded_transport_provider",
    }, obj
    assert provider_caps.get("transport_family") in {
        "sample_webrtc",
        "embedded_transport_primitives",
    }, obj
    if provider_name == "agentd_builtin_embedded_transport_provider":
        assert provider_caps.get("audio_drain") is True, obj
        assert provider_caps.get("audio_owner_handoff") is True, obj
        assert provider_caps.get("audio_submit") is True, obj
        assert provider_caps.get("audio_outbound_pcmu") is True, obj
        assert provider_caps.get("audio_outbound_pcma") is True, obj
        assert isinstance(provider_caps.get("audio_outbound_opus"), bool), obj
        assert provider_caps.get("rtp_transmit") is True, obj
        assert provider_caps.get("rtcp_receiver_report") is True, obj
    assert provider_caps.get("real_media_engine") is False, obj
    assert peer.get("native_media_supported") is expected_native_supported, obj
    assert peer.get("native_media_active") is False, obj
    assert peer.get("rtp_packets_sent", 0) == 0, obj
    assert peer.get("rtp_payload_bytes_sent", 0) == 0, obj
    assert peer.get("audio_outbound_frames_sent", 0) == 0, obj
    assert peer.get("audio_pcm_samples_submitted_total", 0) == 0, obj
    assert peer.get("audio_last_outbound_samples", 0) == 0, obj
    assert peer.get("audio_drain_events_total", 0) == 0, obj
    assert peer.get("audio_pcm_samples_drained_total", 0) == 0, obj
    assert peer.get("audio_pcm_samples_owned", 0) == 0, obj
    assert peer.get("audio_process_events_total", 0) == 0, obj
    assert peer.get("audio_pcm_samples_processed_total", 0) == 0, obj
    assert peer.get("audio_last_process_samples", 0) == 0, obj
    assert peer.get("audio_render_events_total", 0) == 0, obj
    assert peer.get("audio_pcm_samples_rendered_total", 0) == 0, obj
    assert peer.get("audio_last_render_samples", 0) == 0, obj
    assert peer.get("audio_playback_enabled") is False, obj
    assert peer.get("audio_playback_events_total", 0) == 0, obj
    assert peer.get("audio_pcm_samples_played_total", 0) == 0, obj
    assert not peer.get("audio_playback_device_name"), obj
    assert not peer.get("audio_render_wav_path"), obj
else:
    assert peer.get("native_media_supported") is False, obj
    assert peer.get("native_media_active") is False, obj
sid = peer.get("broker_session_id")
assert sid, obj
print(sid)
PY
)"

REMOTE_BYE_SENT="0"
USE_BROWSER_NATIVE_PLUGIN_SMOKE="0"
if [[ "${BUILTIN_MODE}" == "native_plugin" && "${BUILTIN_NATIVE_LIBRARY}" == *"embedded_transport"* ]]; then
  if ROOT_PATH="${ROOT}" node - <<'JS' >/dev/null 2>&1
const path = require("path");
try {
  const { chromium } = require(path.resolve(process.env.ROOT_PATH, "ui/node_modules/playwright"));
  process.exit(chromium ? 0 : 1);
} catch (_) {
  process.exit(1);
}
JS
  then
    USE_BROWSER_NATIVE_PLUGIN_SMOKE="1"
  fi
fi

if [[ "${USE_BROWSER_NATIVE_PLUGIN_SMOKE}" == "1" ]]; then
  ROOT_PATH="${ROOT}" node - "${BROKER_PORT}" "${BROKER_SESSION_ID}" "${DAEMON_URL}" "${DAEMON_TOKEN}" "${SESSION_ID}" <<'JS' >>"${LOG_FILE}" 2>&1
const path = require("path");
const { chromium } = require(path.resolve(process.env.ROOT_PATH, "ui/node_modules/playwright"));

const [brokerPort, brokerSessionId, daemonUrl, daemonToken, sessionId] = process.argv.slice(2);
const brokerUrl = `http://127.0.0.1:${brokerPort}`;
const senderTag = "webui-browser-full-duplex-peer";

function join(base, suffix) {
  return base.replace(/\/+$/, "") + (suffix.startsWith("/") ? suffix : "/" + suffix);
}

async function sendSignal(type, payload) {
  const bodyPayload = payload && typeof payload === "object"
    ? { ...payload, sender_tag: senderTag }
    : { sender_tag: senderTag };
  const res = await fetch(join(brokerUrl, `/v1/audio/sessions/${encodeURIComponent(brokerSessionId)}/signal`), {
    method: "POST",
    headers: {
      Authorization: "Bearer audio-webui-token",
      "Content-Type": "application/json",
    },
    body: JSON.stringify({ type, payload: bodyPayload }),
  });
  if (!res.ok) {
    throw new Error(`send signal ${type} failed: http ${res.status} ${await res.text()}`);
  }
}

async function fetchPeerStatus() {
  const res = await fetch(
    join(daemonUrl, `/api/v1/session/voice_webrtc_peer?session_id=${encodeURIComponent(sessionId)}`),
    { headers: { Authorization: `Bearer ${daemonToken}` } },
  );
  if (!res.ok) throw new Error(`agentd status failed: http ${res.status} ${await res.text()}`);
  return await res.json();
}

async function run() {
  const browser = await chromium.launch({
    headless: true,
    args: ["--autoplay-policy=no-user-gesture-required"],
  });
  const page = await browser.newPage();
  await page.exposeFunction("__agentdBuiltinBrowserSendSignal", async (type, payload) => {
    await sendSignal(type, payload || {});
  });
  await page.setContent(`<!doctype html><html><body><script>
  (() => {
    const state = {
      answerSeen: false,
      remoteDescriptionSet: false,
      sentCandidateCount: 0,
      receivedCandidateCount: 0,
      remoteTrackCount: 0,
      connectionState: "new",
      iceConnectionState: "new",
      signalingState: "stable",
      outboundPacketsSent: 0,
      outboundBytesSent: 0,
      inboundPacketsReceived: 0,
      inboundBytesReceived: 0,
      localOfferSdp: "",
      answerSdp: "",
      dataChannelCreated: false,
      extraAudioTransceiverCreated: false,
      emptyCandidateSent: false,
      codecPreferenceApplied: false,
      preferredCodecName: "",
      error: "",
    };
    let peer = null;
    let audioCtx = null;
    let oscillator = null;
    let gain = null;
    let dest = null;
    let remoteAudio = null;
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

    async function waitForIceGatheringComplete(pc) {
      if (pc.iceGatheringState === "complete") return;
      await new Promise((resolve) => {
        const timeout = setTimeout(resolve, 3000);
        pc.addEventListener("icegatheringstatechange", () => {
          if (pc.iceGatheringState === "complete") {
            clearTimeout(timeout);
            resolve();
          }
        });
      });
    }

    async function ensurePeer() {
      if (peer) return peer;
      audioCtx = new (window.AudioContext || window.webkitAudioContext)();
      dest = audioCtx.createMediaStreamDestination();
      gain = audioCtx.createGain();
      gain.gain.value = 0.035;
      oscillator = audioCtx.createOscillator();
      oscillator.type = "sine";
      oscillator.frequency.value = 440;
      oscillator.connect(gain);
      gain.connect(dest);
      oscillator.start();
      if (typeof audioCtx.resume === "function") await audioCtx.resume();

      peer = new RTCPeerConnection();
      const audioTrack = dest.stream.getAudioTracks()[0];
      const activeAudioTransceiver = peer.addTransceiver(audioTrack, {
        direction: "sendrecv",
        streams: [dest.stream],
      });
      const audioCaps = typeof RTCRtpSender !== "undefined" && RTCRtpSender.getCapabilities
        ? RTCRtpSender.getCapabilities("audio")
        : null;
      const g711Codecs = (audioCaps && Array.isArray(audioCaps.codecs) ? audioCaps.codecs : [])
        .filter((codec) => {
          const mimeType = String(codec.mimeType || "").toLowerCase();
          return (mimeType === "audio/pcmu" || mimeType === "audio/pcma") &&
            Number(codec.clockRate || 0) === 8000;
        });
      const preferredG711 = g711Codecs.find((codec) => String(codec.mimeType || "").toLowerCase() === "audio/pcmu") ||
        g711Codecs.find((codec) => String(codec.mimeType || "").toLowerCase() === "audio/pcma") ||
        null;
      if (preferredG711 && typeof activeAudioTransceiver.setCodecPreferences === "function") {
        activeAudioTransceiver.setCodecPreferences([preferredG711]);
        state.codecPreferenceApplied = true;
        state.preferredCodecName = String(preferredG711.mimeType || "").split("/").pop().toUpperCase();
      }
      peer.addTransceiver("audio", { direction: "recvonly" });
      state.extraAudioTransceiverCreated = true;
      peer.createDataChannel("agentd-native-voice-edge-validation");
      state.dataChannelCreated = true;
      remoteAudio = document.createElement("audio");
      remoteAudio.autoplay = true;
      document.body.appendChild(remoteAudio);
      peer.onconnectionstatechange = () => { state.connectionState = peer.connectionState || "new"; };
      peer.oniceconnectionstatechange = () => { state.iceConnectionState = peer.iceConnectionState || "new"; };
      peer.onsignalingstatechange = () => { state.signalingState = peer.signalingState || "stable"; };
      peer.onicecandidate = async (event) => {
        if (!event.candidate) return;
        state.sentCandidateCount += 1;
        await window.__agentdBuiltinBrowserSendSignal("candidate", candidatePayload(event.candidate));
      };
      peer.ontrack = (event) => {
        state.remoteTrackCount += 1;
        remoteAudio.srcObject = event.streams && event.streams[0]
          ? event.streams[0]
          : new MediaStream([event.track]);
      };
      return peer;
    }

    async function startOffer() {
      const pc = await ensurePeer();
      const offer = await pc.createOffer();
      await pc.setLocalDescription(offer);
      await waitForIceGatheringComplete(pc);
      state.localOfferSdp = pc.localDescription.sdp || "";
      await window.__agentdBuiltinBrowserSendSignal("offer", {
        type: pc.localDescription.type,
        sdp: pc.localDescription.sdp,
      });
      await window.__agentdBuiltinBrowserSendSignal("candidate", {
        candidate: "",
        sdpMid: "0",
        sdpMLineIndex: 0,
      });
      state.emptyCandidateSent = true;
    }

    async function handleSignal(ev) {
      const pc = await ensurePeer();
      if (!ev || typeof ev !== "object") return;
      if (ev.type === "answer") {
        const payload = ev.payload || {};
        if (!payload.sdp) return;
        state.answerSeen = true;
        const answerSdp = String(payload.sdp).endsWith("\\n") ? String(payload.sdp) : String(payload.sdp) + "\\r\\n";
        state.answerSdp = answerSdp;
        try {
          await pc.setRemoteDescription({ type: payload.type || "answer", sdp: answerSdp });
        } catch (err) {
          throw new Error(
            "browser remote answer SDP rejected: " +
            (err && err.message ? err.message : String(err)) +
            "\\nsignalingState=" +
            pc.signalingState +
            "\\nlocalDescription=" +
            (pc.localDescription && pc.localDescription.sdp ? pc.localDescription.sdp : "") +
            "\\nanswer=" +
            "\\n" +
            answerSdp,
          );
        }
        state.remoteDescriptionSet = true;
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
      }
    }

    async function getState() {
      if (peer) {
        const stats = await peer.getStats();
        for (const report of stats.values()) {
          const mediaType = report.mediaType || report.kind || "";
          if (report.type === "outbound-rtp" && mediaType === "audio") {
            state.outboundPacketsSent = Math.max(state.outboundPacketsSent, report.packetsSent || 0);
            state.outboundBytesSent = Math.max(state.outboundBytesSent, report.bytesSent || 0);
          }
          if (report.type === "inbound-rtp" && mediaType === "audio") {
            state.inboundPacketsReceived = Math.max(
              state.inboundPacketsReceived,
              report.packetsReceived || 0,
            );
            state.inboundBytesReceived = Math.max(state.inboundBytesReceived, report.bytesReceived || 0);
          }
        }
      }
      return { ...state };
    }

    window.__agentdBuiltinBrowserStartOffer = startOffer;
    window.__agentdBuiltinBrowserHandleSignal = handleSignal;
    window.__agentdBuiltinBrowserGetState = getState;
  })();
  </script></body></html>`);

  const streamAbort = new AbortController();
  let streamReadyResolve;
  let streamReadyReject;
  const streamReady = new Promise((resolve, reject) => {
    streamReadyResolve = resolve;
    streamReadyReject = reject;
  });
  const streamPromise = (async () => {
    const res = await fetch(join(brokerUrl, `/v1/audio/sessions/${encodeURIComponent(brokerSessionId)}/signal/stream`), {
      method: "GET",
      headers: {
        Authorization: "Bearer audio-webui-token",
        Accept: "text/event-stream",
      },
      signal: streamAbort.signal,
    });
    if (!res.ok || !res.body) throw new Error(`signal stream failed: http ${res.status}`);
    streamReadyResolve();
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
        try {
          ev = JSON.parse(raw);
        } catch (_) {
          continue;
        }
        const payload = ev && ev.payload && typeof ev.payload === "object" ? ev.payload : {};
        if (String(payload.sender_tag || "").trim() === senderTag) continue;
        await page.evaluate((msg) => window.__agentdBuiltinBrowserHandleSignal(msg), ev);
      }
    }
  })().catch((err) => {
    if (streamReadyReject) streamReadyReject(err);
    if (err && (err.name === "AbortError" || String(err).includes("AbortError"))) return;
    throw err;
  });

  await streamReady;
  await page.evaluate(() => window.__agentdBuiltinBrowserStartOffer());

  const deadline = Date.now() + 25000;
  let finalState = null;
  let finalPeer = null;
  while (Date.now() < deadline) {
    finalState = await page.evaluate(() => window.__agentdBuiltinBrowserGetState());
    finalPeer = (await fetchPeerStatus()).peer || {};
    if (
      finalState.answerSeen &&
      finalState.remoteDescriptionSet &&
      finalState.outboundPacketsSent > 0 &&
      finalState.inboundPacketsReceived > 0 &&
      finalPeer.native_media_active === true &&
      (finalPeer.rtp_packets_received || 0) > 0 &&
      (finalPeer.rtp_packets_sent || 0) > 0 &&
      (finalPeer.audio_frames_decoded || 0) > 0 &&
      (finalPeer.audio_outbound_frames_sent || 0) > 0 &&
      (finalPeer.rtcp_packets_sent || 0) > 0 &&
      (finalPeer.media_remote_candidates_seen || 0) > 0
    ) {
      break;
    }
    await new Promise((resolve) => setTimeout(resolve, 200));
  }

  if (
    !finalState ||
    !finalPeer ||
    !finalState.answerSeen ||
    !finalState.remoteDescriptionSet ||
    finalState.outboundPacketsSent <= 0 ||
    finalState.inboundPacketsReceived <= 0 ||
    finalPeer.native_media_active !== true ||
    (finalPeer.rtp_packets_received || 0) <= 0 ||
    (finalPeer.rtp_packets_sent || 0) <= 0 ||
    (finalPeer.audio_frames_decoded || 0) <= 0 ||
    (finalPeer.audio_outbound_frames_sent || 0) <= 0 ||
    (finalPeer.rtcp_packets_sent || 0) <= 0 ||
    (finalPeer.media_remote_candidates_seen || 0) <= 0
  ) {
    throw new Error(`native browser full-duplex media did not converge: ${JSON.stringify({
      browser: finalState,
      peer: finalPeer,
    })}`);
  }
  if (!finalState.dataChannelCreated || !finalState.extraAudioTransceiverCreated || !finalState.emptyCandidateSent) {
    throw new Error(`native browser edge offer was not constructed: ${JSON.stringify(finalState)}`);
  }
  if (
    finalState.codecPreferenceApplied &&
    String(finalPeer.audio_outbound_codec_name || "").toUpperCase() !== finalState.preferredCodecName
  ) {
    throw new Error(`native browser codec preference was not honored: ${JSON.stringify({
      browser: finalState,
      peer: finalPeer,
    })}`);
  }
  if (
    !/m=application 0 [^\r\n]*webrtc-datachannel/.test(finalState.answerSdp || "") ||
    !/m=audio 0 /.test(finalState.answerSdp || "") ||
    !/m=audio 9 /.test(finalState.answerSdp || "")
  ) {
    throw new Error(`native browser edge offer answer did not reject unsupported sections: ${finalState.answerSdp || ""}`);
  }
  const bundleLine = String(finalState.answerSdp || "")
    .split(/\r?\n/)
    .find((line) => line.startsWith("a=group:BUNDLE"));
  if (!bundleLine || bundleLine.trim().split(/\s+/).length !== 2) {
    throw new Error(`native browser edge answer did not keep BUNDLE scoped to one active mid: ${finalState.answerSdp || ""}`);
  }

  await sendSignal("bye", { reason: "browser_full_duplex_done" });
  streamAbort.abort();
  await browser.close();
  await streamPromise.catch(() => {});
  process.stdout.write(JSON.stringify({ ok: true, browser: finalState, peer: finalPeer }) + "\n");
}

run().catch((err) => {
  process.stderr.write(String(err) + "\n");
  process.exit(1);
});
JS
  REMOTE_BYE_SENT="1"
else
  ANSWER_FILE="${LOG_DIR}/agentd_voice_builtin_answer_${DAEMON_PORT}.json"
  python3 - "${BROKER_PORT}" "${BROKER_SESSION_ID}" "${ANSWER_FILE}" <<'PY' >>"${LOG_FILE}" 2>&1 &
import json
import sys
import urllib.request

port, session_id, answer_file = sys.argv[1], sys.argv[2], sys.argv[3]
req = urllib.request.Request(
    f"http://127.0.0.1:{port}/v1/audio/sessions/{session_id}/signal/stream",
    headers={
        "Authorization": "Bearer audio-webui-token",
        "Accept": "text/event-stream",
    },
)
with urllib.request.urlopen(req, timeout=15) as resp:
    for raw in resp:
        line = raw.decode("utf-8", "replace").strip()
        if not line.startswith("data:"):
            continue
        payload = line[5:].strip()
        if not payload:
            continue
        msg = json.loads(payload)
        if msg.get("type") == "answer" and (msg.get("payload") or {}).get("sender_tag") == "agentd_runtime_peer":
            with open(answer_file, "w", encoding="utf-8") as fh:
                json.dump(msg, fh)
            raise SystemExit(0)
raise SystemExit("answer not observed")
PY
  LISTENER_PID=$!

  sleep 0.5
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Authorization: Bearer audio-webui-token" \
    -H "Content-Type: application/json" \
    -d '{"type":"offer","payload":{"type":"offer","sdp":"stub-offer","sender_tag":"webui-peer"}}' \
    "http://127.0.0.1:${BROKER_PORT}/v1/audio/sessions/${BROKER_SESSION_ID}/signal" >/dev/null

  wait "${LISTENER_PID}"

  python3 - "${ANSWER_FILE}" "${BUILTIN_MODE}" <<'PY'
import json, sys
msg = json.load(open(sys.argv[1], "r", encoding="utf-8"))
mode = sys.argv[2]
payload = msg.get("payload") or {}
assert msg.get("type") == "answer", msg
assert payload.get("type") == "answer", msg
if mode == "signaling_stub":
    assert payload.get("sdp") == "stub-answer", msg
else:
    sdp = payload.get("sdp") or ""
    assert sdp, msg
    if sdp != "agentd-builtin-sample-answer":
        assert "a=ice-ufrag:" in sdp, msg
assert payload.get("sender_tag") == "agentd_runtime_peer", msg
PY
fi

if [[ "${REMOTE_BYE_SENT}" == "0" ]]; then
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Authorization: Bearer audio-webui-token" \
    -H "Content-Type: application/json" \
    -d '{"type":"bye","payload":{"reason":"webui_done","sender_tag":"webui-peer"}}' \
    "http://127.0.0.1:${BROKER_PORT}/v1/audio/sessions/${BROKER_SESSION_ID}/signal" >/dev/null
fi

STATUS_JSON=""
for _ in $(seq 1 40); do
  STATUS_JSON="$(curl -fsS --noproxy "*" --max-time 10 \
    -H "Authorization: Bearer ${DAEMON_TOKEN}" \
    "${DAEMON_URL}/api/v1/session/voice_webrtc_peer?session_id=$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${SESSION_ID}")")"
  if python3 - "${STATUS_JSON}" <<'PY'
import json, sys
obj = json.loads(sys.argv[1])
peer = obj.get("peer")
last_stdout = (peer or {}).get("last_stdout") or {}
if (
    peer
    and peer.get("running") is False
    and last_stdout.get("event") == "builtin_runtime_stopped"
    and last_stdout.get("remote_bye_received") is True
    and peer.get("media_engine_state") == "stopped"
    and peer.get("media_remote_offers_seen") == 1
    and peer.get("media_answers_sent") == 1
    and peer.get("media_remote_byes_seen") == 1
):
    raise SystemExit(0)
raise SystemExit(1)
PY
  then
    break
  fi
  sleep 0.25
done

STOP_JSON="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H "Content-Type: application/json" \
  -d "{\"session_id\":\"${SESSION_ID}\",\"action\":\"stop\",\"broker_token\":\"audio-agentd-token\"}" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"

python3 - "${STOP_JSON}" "${BROKER_PORT}" "${BROKER_SESSION_ID}" <<'PY'
import json
import sys
import urllib.request
import urllib.error

obj = json.loads(sys.argv[1])
port = sys.argv[2]
broker_session_id = sys.argv[3]
assert obj.get("ok") is True, obj
assert obj.get("reason") == "not_running", obj
assert obj.get("broker_session_delete_attempted") is True, obj
assert obj.get("broker_session_deleted") is True, obj
peer = obj.get("peer") or {}
assert peer.get("runtime_kind") == "builtin", obj
assert peer.get("running") is False, obj
assert peer.get("media_engine_state") == "stopped", obj
assert peer.get("media_remote_offers_seen") == 1, obj
assert peer.get("media_answers_sent") == 1, obj
assert peer.get("media_remote_byes_seen") == 1, obj
assert peer.get("media_events_total", 0) >= 5, obj
provider = peer.get("native_media_provider") or {}
if provider:
    assert provider.get("name") in {
        "agentd_builtin_sample_provider",
        "agentd_builtin_embedded_transport_provider",
    }, obj
req = urllib.request.Request(
    f"http://127.0.0.1:{port}/v1/audio/sessions/{broker_session_id}",
    headers={"Authorization": "Bearer audio-agentd-token"},
)
try:
    urllib.request.urlopen(req, timeout=10)
    raise SystemExit("expected deleted broker session")
except urllib.error.HTTPError as exc:
    if exc.code != 404:
        raise
PY

echo "agentd_session_voice_webrtc_peer_builtin_smoke ${BUILTIN_MODE} OK"
