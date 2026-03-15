#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "usage: $0 <agentd>" >&2
  exit 2
fi

if ! command -v node >/dev/null 2>&1; then
  echo "SKIP: node not found" >&2
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

ROOT="$(agentd_smoke_project_root)"
PEER_TOOL="${ROOT}/tools/agentd_audio_webrtc_peer.js"
if [[ ! -f "${PEER_TOOL}" ]]; then
  echo "SKIP: peer tool not found (${PEER_TOOL})" >&2
  exit 77
fi

if ! ROOT_PATH="${ROOT}" node - <<'JS' >/dev/null 2>&1
const path = require('path');
try {
  const { chromium } = require(path.resolve(process.env.ROOT_PATH, 'ui/node_modules/playwright'));
  process.exit(chromium ? 0 : 1);
} catch (_) {
  process.exit(1);
}
JS
then
  echo "SKIP: playwright chromium dependency not available" >&2
  exit 77
fi

PG_LIB="${ROOT}/tests/lib/pg_test_lib.sh"
if [[ -f "${PG_LIB}" ]]; then
  # shellcheck disable=SC1090
  source "${PG_LIB}"
fi

LOG_DIR="${ROOT}/build"
mkdir -p "${LOG_DIR}"
LOG_FILE="${LOG_DIR}/agentd_session_voice_webrtc_peer_runtime_smoke.log"

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

POSTGRES_NAME="agentd_session_voice_webrtc_peer_runtime_smoke"
BROKER_PORT=""
PORT_DAEMON=""
PG_PORT=""
BROKER_PID=""

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
fi
BROKER_PORT="$(pick_port)"
PORT_DAEMON="$(pick_port)"

if [[ "${USE_DOCKER}" == "1" ]]; then
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

CLIENT_AUTH_JSON="${LOG_DIR}/broker_audio_runtime_client_auth.json"
cat >"${CLIENT_AUTH_JSON}" <<JSON
{
  "clients": [
    {
      "client_id": "audio-agentd",
      "token": "audio-agentd-token",
      "admin": true
    },
    {
      "client_id": "audio-webui",
      "token": "audio-webui-token",
      "admin": true
    }
  ]
}
JSON

BROKER_BIN="${LOG_DIR}/agentd-broker-audio-runtime"
(
  cd "${ROOT}/broker"
  go build -trimpath -o "${BROKER_BIN}" ./cmd/agentd-broker
) >>"${LOG_FILE}" 2>&1

DSN="${PG_DSN_OVERRIDE}"
if [[ -z "${DSN}" ]]; then
  if [[ "${USE_LOCAL_PG}" == "1" ]]; then
    DSN="${PG_TEST_DSN}"
  else
    DSN="postgres://postgres:postgres@127.0.0.1:${PG_PORT}/postgres?sslmode=disable"
  fi
fi

"${BROKER_BIN}" \
  --listen "127.0.0.1:${BROKER_PORT}" \
  --db-dsn "${DSN}" \
  --client-auth-file "${CLIENT_AUTH_JSON}" >>"${LOG_FILE}" 2>&1 &
BROKER_PID=$!

for _ in $(seq 1 60); do
  if curl -fsS "http://127.0.0.1:${BROKER_PORT}/healthz" >/dev/null 2>&1; then
    break
  fi
  sleep 0.2
done
if ! curl -fsS "http://127.0.0.1:${BROKER_PORT}/healthz" >/dev/null 2>&1; then
  echo "Broker did not become ready; see ${LOG_FILE}" >&2
  exit 1
fi

HOST="127.0.0.1"
DAEMON_TOKEN="agentd-audio-runtime-token"
AGENTD_AUDIO_WEBRTC_PEER_TOOL="${PEER_TOOL}" \
AGENTD_AUTH_TOKEN="${DAEMON_TOKEN}" \
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_session_voice_webrtc_peer_runtime_smoke" >>"${LOG_FILE}" 2>&1
DAEMON_URL="http://${HOST}:${PORT_DAEMON}"

for _ in $(seq 1 100); do
  if curl -fsS --noproxy "*" --max-time 10 \
    -H "Authorization: Bearer ${DAEMON_TOKEN}" \
    "${DAEMON_URL}/api/v1/health" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done
if ! curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  "${DAEMON_URL}/api/v1/health" >/dev/null 2>&1; then
  echo "agentd did not become ready" >&2
  exit 1
fi

SESSION_DB_ID="agentd_session_voice_webrtc_peer_runtime_$(date +%s)_$RANDOM"
curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${SESSION_DB_ID}\"}" \
  "${DAEMON_URL}/api/v1/session/new" >/dev/null

create_broker_session() {
  python3 - <<PY
import http.client, json, sys
host = "127.0.0.1"
port = ${BROKER_PORT}
conn = http.client.HTTPConnection(host, port, timeout=5)
body = json.dumps({"agent_id": "a-1", "mode": "webrtc"})
conn.request("POST", "/v1/audio/sessions", body=body, headers={
    "Authorization": "Bearer audio-webui-token",
    "Content-Type": "application/json",
})
resp = conn.getresponse()
data = resp.read().decode("utf-8")
if resp.status != 200:
  print(data, file=sys.stderr)
  raise SystemExit(1)
obj = json.loads(data)
if not obj.get("ok"):
  print(data, file=sys.stderr)
  raise SystemExit(1)
print(obj.get("session_id", ""))
PY
}

wait_voice_peer_ready() {
  local session_id="$1"
  local expect_running="${2:-1}"
  local out_var="${3:-}"
  local status_json=""
  for _ in $(seq 1 120); do
    status_json="$(curl -fsS --noproxy "*" --max-time 10 \
      -H "Authorization: Bearer ${DAEMON_TOKEN}" \
      "${DAEMON_URL}/api/v1/session/voice_webrtc_peer?session_id=$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${session_id}")")"
    if python3 - <<PY
import json, sys
obj = json.loads(r'''${status_json}''')
peer = obj.get("peer")
expect_running = ${expect_running}
if not isinstance(peer, dict):
  raise SystemExit(1)
running = bool(peer.get("running"))
ready = bool(peer.get("ready"))
if expect_running:
  raise SystemExit(0 if running and ready else 1)
raise SystemExit(0 if not running else 1)
PY
    then
      if [[ -n "${out_var}" ]]; then
        printf -v "${out_var}" '%s' "${status_json}"
      fi
      return 0
    fi
    sleep 0.1
  done
  echo "voice peer status did not reach expected state: ${status_json}" >&2
  return 1
}

wait_broker_session_deleted() {
  local session_id="$1"
  local code=""
  for _ in $(seq 1 100); do
    code="$(curl -sS --noproxy "*" --max-time 10 -o /dev/null -w '%{http_code}' \
      "http://127.0.0.1:${BROKER_PORT}/v1/audio/sessions/${session_id}" \
      -H "Authorization: Bearer audio-webui-token")"
    if [[ "${code}" == "404" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected deleted audio session after teardown, got status ${code}" >&2
  return 1
}

run_receiver_peer() {
  local session_id="$1"
  ROOT_PATH="${ROOT}" node - <<JS >>"${LOG_FILE}" 2>&1
const path = require("path");
const { chromium } = require(path.resolve(process.env.ROOT_PATH, "ui/node_modules/playwright"));

const brokerUrl = "http://127.0.0.1:${BROKER_PORT}";
const sessionId = "${session_id}";
const token = "audio-webui-token";
const senderTag = "webui_playwright_peer";

function join(base, suffix) {
  return base.replace(/\\/+$/, "") + (suffix.startsWith("/") ? suffix : "/" + suffix);
}

async function sendSignal(type, payload) {
  const bodyPayload = payload && typeof payload === "object" ? { ...payload, sender_tag: senderTag } : { sender_tag: senderTag };
  const res = await fetch(join(brokerUrl, \`/v1/audio/sessions/\${encodeURIComponent(sessionId)}/signal\`), {
    method: "POST",
    headers: {
      Authorization: \`Bearer \${token}\`,
      "Content-Type": "application/json",
    },
    body: JSON.stringify({ type, payload: bodyPayload }),
  });
  if (!res.ok) throw new Error(\`send \${type} failed: http \${res.status} \${await res.text()}\`);
}

async function run() {
  const browser = await chromium.launch({ headless: true, args: ["--autoplay-policy=no-user-gesture-required"] });
  const page = await browser.newPage();
  await page.exposeFunction("__webuiSendSignal", async (type, payload) => {
    await sendSignal(type, payload || {});
  });
  await page.setContent(\`<!doctype html><html><body><script>
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
      peer = new RTCPeerConnection();
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
  </script></body></html>\`);

  const streamAbort = new AbortController();
  const streamPromise = (async () => {
    const res = await fetch(join(brokerUrl, \`/v1/audio/sessions/\${encodeURIComponent(sessionId)}/signal/stream\`), {
      method: "GET",
      headers: { Authorization: \`Bearer \${token}\`, Accept: "text/event-stream" },
      signal: streamAbort.signal,
    });
    if (!res.ok || !res.body) throw new Error(\`signal stream failed: http \${res.status}\`);
    const reader = res.body.getReader();
    const decoder = new TextDecoder();
    let buffer = "";
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      buffer += decoder.decode(value, { stream: true });
      for (;;) {
        const idx = buffer.indexOf("\\n");
        if (idx < 0) break;
        let line = buffer.slice(0, idx);
        buffer = buffer.slice(idx + 1);
        if (line.endsWith("\\r")) line = line.slice(0, -1);
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
  process.stdout.write(JSON.stringify({ ok: true, state: finalState }) + "\\n");
}

run().catch((err) => {
  process.stderr.write(String(err) + "\\n");
  process.exit(1);
});
JS
}

BROKER_SESSION_ID="$(create_broker_session)"
if [[ -z "${BROKER_SESSION_ID}" ]]; then
  echo "failed to create broker audio session" >&2
  exit 1
fi

start_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${SESSION_DB_ID}",
  "action": "start",
  "broker_session_id": "${BROKER_SESSION_ID}",
  "broker_url": "http://127.0.0.1:${BROKER_PORT}",
  "broker_token": "audio-agentd-token",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 440
}))
PY
)" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${start_resp}''')
if not obj.get("ok") or not obj.get("started"):
  print("voice_webrtc_peer start failed", obj, file=sys.stderr)
  raise SystemExit(1)
peer = obj.get("peer") or {}
if not peer.get("running"):
  print("voice peer did not report running", obj, file=sys.stderr)
  raise SystemExit(1)
PY

status_json=""
wait_voice_peer_ready "${SESSION_DB_ID}" 1 status_json

run_receiver_peer "${BROKER_SESSION_ID}"

SESSION_JSON="$(curl -fsS --noproxy "*" --max-time 10 \
  "http://127.0.0.1:${BROKER_PORT}/v1/audio/sessions/${BROKER_SESSION_ID}" \
  -H "Authorization: Bearer audio-webui-token")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${SESSION_JSON}''')
if not obj.get("ok"):
  print("session get failed", obj, file=sys.stderr)
  raise SystemExit(1)
sess = obj.get("session") or {}
if (sess.get("signal_count") or 0) < 4:
  print("signal_count too small", sess, file=sys.stderr)
  raise SystemExit(1)
PY

curl -fsS --noproxy "*" --max-time 10 \
  "http://127.0.0.1:${BROKER_PORT}/v1/audio/sessions/${BROKER_SESSION_ID}/signal" \
  -H "Authorization: Bearer audio-webui-token" \
  -H "Content-Type: application/json" \
  -d '{"type":"bye","payload":{"reason":"webui_done","sender_tag":"webui_playwright_peer"}}' >/dev/null

stopped_json=""
wait_voice_peer_ready "${SESSION_DB_ID}" 0 stopped_json

python3 - <<PY
import json, sys
obj = json.loads(r'''${stopped_json}''')
peer = obj.get("peer") or {}
last = peer.get("last_stdout") or {}
if peer.get("running"):
  print("peer still running", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("exit_code") not in (0, None):
  print("unexpected exit_code", obj, file=sys.stderr)
  raise SystemExit(1)
if not last.get("closed_by_remote"):
  print("expected closed_by_remote final state", obj, file=sys.stderr)
  raise SystemExit(1)
PY

wait_broker_session_deleted "${BROKER_SESSION_ID}"

BROKER_SESSION_ID2="$(create_broker_session)"
start_resp2="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${SESSION_DB_ID}",
  "action": "start",
  "broker_session_id": "${BROKER_SESSION_ID2}",
  "broker_url": "http://127.0.0.1:${BROKER_PORT}",
  "broker_token": "audio-agentd-token",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 523
}))
PY
)" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${start_resp2}''')
if not obj.get("ok") or not obj.get("started"):
  print("voice_webrtc_peer second start failed", obj, file=sys.stderr)
  raise SystemExit(1)
PY

wait_voice_peer_ready "${SESSION_DB_ID}" 1 status_json

stop_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${SESSION_DB_ID}\",\"action\":\"stop\"}" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${stop_resp}''')
if not obj.get("ok") or not obj.get("stopped"):
  print("voice_webrtc_peer stop failed", obj, file=sys.stderr)
  raise SystemExit(1)
PY

wait_voice_peer_ready "${SESSION_DB_ID}" 0 stopped_json

python3 - <<PY
import json, sys
obj = json.loads(r'''${stopped_json}''')
peer = obj.get("peer") or {}
last = peer.get("last_stdout") or {}
if peer.get("running"):
  print("peer still running after stop", obj, file=sys.stderr)
  raise SystemExit(1)
if not last.get("stopped"):
  print("expected stopped final state after stop", obj, file=sys.stderr)
  raise SystemExit(1)
PY

wait_broker_session_deleted "${BROKER_SESSION_ID2}"

echo "agentd_session_voice_webrtc_peer_runtime_smoke OK"
