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
SESSION_DB_PATH="${LOG_DIR}/agentd_session_voice_webrtc_peer_runtime_smoke_${PORT_DAEMON}.sqlite"

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
VOICE_BROKER_URL="http://127.0.0.1:${BROKER_PORT}"
VOICE_BROKER_TOKEN="audio-agentd-token"

start_agentd_with_voice_defaults() {
  export AGENTD_AUTH_TOKEN="${DAEMON_TOKEN}"
  export AGENTD_AUDIO_WEBRTC_DEFAULT_RUNTIME_KIND="bundled"
  export AGENTD_AUDIO_WEBRTC_BROKER_URL="${VOICE_BROKER_URL}"
  export AGENTD_AUDIO_WEBRTC_BROKER_TOKEN="${VOICE_BROKER_TOKEN}"
  agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_session_voice_webrtc_peer_runtime_smoke" >>"${LOG_FILE}" 2>&1
  unset AGENTD_AUTH_TOKEN
  unset AGENTD_AUDIO_WEBRTC_DEFAULT_RUNTIME_KIND
  unset AGENTD_AUDIO_WEBRTC_BROKER_URL
  unset AGENTD_AUDIO_WEBRTC_BROKER_TOKEN
}

start_agentd_without_voice_defaults() {
  export AGENTD_AUTH_TOKEN="${DAEMON_TOKEN}"
  unset AGENTD_AUDIO_WEBRTC_DEFAULT_RUNTIME_KIND
  unset AGENTD_AUDIO_WEBRTC_BROKER_URL
  unset AGENTD_AUDIO_WEBRTC_BROKER_TOKEN
  agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_session_voice_webrtc_peer_runtime_smoke" >>"${LOG_FILE}" 2>&1
  unset AGENTD_AUTH_TOKEN
}

start_agentd_with_voice_defaults
DAEMON_URL="http://${HOST}:${PORT_DAEMON}"

wait_daemon_ready() {
  for _ in $(seq 1 100); do
    if curl -fsS --noproxy "*" --max-time 10 \
      -H "Authorization: Bearer ${DAEMON_TOKEN}" \
      "${DAEMON_URL}/api/v1/health" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "agentd did not become ready" >&2
  return 1
}

wait_daemon_stopped() {
  for _ in $(seq 1 100); do
    if ! curl -fsS --noproxy "*" --max-time 2 \
      -H "Authorization: Bearer ${DAEMON_TOKEN}" \
      "${DAEMON_URL}/api/v1/health" >/dev/null 2>&1; then
      if python3 - "${HOST}" "${PORT_DAEMON}" <<'PY'
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
try:
    s.bind((host, port))
except OSError:
    sys.exit(1)
finally:
    s.close()
sys.exit(0)
PY
      then
        return 0
      fi
    fi
    sleep 0.1
  done
  echo "agentd did not stop cleanly" >&2
  return 1
}

restart_agentd() {
  agentd_smoke_stop
  wait_daemon_stopped
  start_agentd_with_voice_defaults
  wait_daemon_ready
}

restart_agentd_without_voice_defaults() {
  agentd_smoke_stop
  wait_daemon_stopped
  start_agentd_without_voice_defaults
  wait_daemon_ready
}

wait_daemon_ready

config_env_defaults="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  "${DAEMON_URL}/api/v1/config")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${config_env_defaults}''')
daemon = obj.get("daemon") or {}
audio = daemon.get("audio_webrtc") or {}
if audio.get("broker_url_default_configured") is not True:
  print("expected broker_url_default_configured from daemon env", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("broker_token_default_configured") is not True:
  print("expected broker_token_default_configured from daemon env", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("default_runtime_kind") != "bundled" or audio.get("default_runtime_kind_source") != "env":
  print("expected env default_runtime_kind before runtime config override", obj, file=sys.stderr)
  raise SystemExit(1)
PY

SESSION_DB_ID="agentd_session_voice_webrtc_peer_runtime_$(date +%s)_$RANDOM"
curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${SESSION_DB_ID}\"}" \
  "${DAEMON_URL}/api/v1/session/new" >/dev/null

wait_voice_peer_ready() {
  local session_id="$1"
  local expect_running="${2:-1}"
  local out_var="${3:-}"
  local expected_runtime_kind="${4:-bundled}"
  local expected_default_runtime_kind="${5:-bundled}"
  local expected_default_runtime_kind_source="${6:-env}"
  local expected_external_available="${7:-0}"
  local status_body=""
  for _ in $(seq 1 120); do
    status_body="$(curl -fsS --noproxy "*" --max-time 10 \
      -H "Authorization: Bearer ${DAEMON_TOKEN}" \
      "${DAEMON_URL}/api/v1/session/voice_webrtc_peer?session_id=$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${session_id}")")"
    if python3 - <<PY
import json, sys
obj = json.loads(r'''${status_body}''')
peer = obj.get("peer")
expect_running = ${expect_running}
expected_runtime_kind = r'''${expected_runtime_kind}'''
expected_default_runtime_kind = r'''${expected_default_runtime_kind}'''
expected_default_runtime_kind_source = r'''${expected_default_runtime_kind_source}'''
expected_external_available = bool(${expected_external_available})
if obj.get("builtin_available") is not False or obj.get("bundled_available") is not True:
  raise SystemExit(1)
if obj.get("external_available") is not expected_external_available:
  raise SystemExit(1)
if obj.get("default_runtime_kind") != expected_default_runtime_kind:
  raise SystemExit(1)
if obj.get("default_runtime_kind_source") != expected_default_runtime_kind_source:
  raise SystemExit(1)
if obj.get("default_runtime_kind_available") is not True:
  raise SystemExit(1)
if obj.get("broker_url_default_configured") is not True or obj.get("broker_token_default_configured") is not True:
  raise SystemExit(1)
if not isinstance(peer, dict):
  raise SystemExit(1)
if peer.get("runtime_kind") != expected_runtime_kind:
  raise SystemExit(1)
running = bool(peer.get("running"))
ready = bool(peer.get("ready"))
if expect_running:
  raise SystemExit(0 if running and ready else 1)
raise SystemExit(0 if not running else 1)
PY
    then
      if [[ -n "${out_var}" ]]; then
        printf -v "${out_var}" '%s' "${status_body}"
      fi
      return 0
    fi
    sleep 0.1
  done
  echo "voice peer status did not reach expected state: ${status_body}" >&2
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

start_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${SESSION_DB_ID}",
  "action": "start",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 60000,
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
if obj.get("builtin_available") is not False or obj.get("bundled_available") is not True:
  print("unexpected runtime defaults", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("external_available") is not False or obj.get("default_runtime_kind") != "bundled":
  print("unexpected runtime defaults", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("default_runtime_kind_source") != "env" or obj.get("default_runtime_kind_available") is not True:
  print("unexpected runtime defaults", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("broker_url_default_configured") is not True or obj.get("broker_token_default_configured") is not True:
  print("expected daemon broker defaults", obj, file=sys.stderr)
  raise SystemExit(1)
peer = obj.get("peer") or {}
if peer.get("runtime_kind") != "bundled":
  print("unexpected peer runtime_kind", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("managed_broker_session") is not True:
  print("expected managed broker session", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("broker_agent_id") != "a-1" or peer.get("broker_deployment_id") != "lab":
  print("unexpected broker ownership fields", obj, file=sys.stderr)
  raise SystemExit(1)
if not peer.get("running"):
  print("voice peer did not report running", obj, file=sys.stderr)
  raise SystemExit(1)
PY

BROKER_SESSION_ID="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${start_resp}''')
peer = obj.get("peer") or {}
sid = str(peer.get("broker_session_id") or "").strip()
if not sid:
  print("missing broker_session_id in start response", file=sys.stderr)
  raise SystemExit(1)
print(sid)
PY
)"

status_json=""
wait_voice_peer_ready "${SESSION_DB_ID}" 1 status_json

restart_agentd
wait_voice_peer_ready "${SESSION_DB_ID}" 1 status_json

python3 - <<PY
import json, sys
obj = json.loads(r'''${status_json}''')
peer = obj.get("peer") or {}
if peer.get("status_source") != "persisted":
  print("expected persisted status source after restart", obj, file=sys.stderr)
  raise SystemExit(1)
if not peer.get("stdout_log_path"):
  print("expected stdout_log_path after restart", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("managed_broker_session") is not True:
  print("expected managed_broker_session after restart", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("broker_agent_id") != "a-1" or peer.get("broker_deployment_id") != "lab":
  print("unexpected persisted broker ownership fields", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("runtime_kind") != "bundled" or not peer.get("running") or not peer.get("ready"):
  print("unexpected recovered runtime state after restart", obj, file=sys.stderr)
  raise SystemExit(1)
PY

already_running_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${SESSION_DB_ID}\",\"action\":\"start\"}" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${already_running_resp}''')
peer = obj.get("peer") or {}
if not obj.get("ok") or not obj.get("already_running"):
  print("expected already_running response after restart", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("status_source") != "persisted":
  print("expected persisted peer on duplicate start after restart", obj, file=sys.stderr)
  raise SystemExit(1)
PY

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

builtin_resp_headers="${LOG_DIR}/voice_webrtc_peer_builtin_headers.txt"
builtin_resp_body="${LOG_DIR}/voice_webrtc_peer_builtin_body.json"
builtin_status="$(curl -sS --noproxy "*" --max-time 10 -o "${builtin_resp_body}" -D "${builtin_resp_headers}" -w '%{http_code}' \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${SESSION_DB_ID}\",\"action\":\"start\",\"runtime_kind\":\"builtin\"}" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"
if [[ "${builtin_status}" != "501" ]]; then
  echo "expected builtin runtime request to return 501, got ${builtin_status}" >&2
  cat "${builtin_resp_body}" >&2
  exit 1
fi
python3 - <<PY
import json, sys
obj = json.load(open(r'''${builtin_resp_body}''', 'r', encoding='utf-8'))
if obj.get("builtin_available") is not False or obj.get("bundled_available") is not True:
  print("unexpected builtin contract response", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("external_available") is not False or obj.get("default_runtime_kind") != "bundled":
  print("unexpected builtin contract response", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("default_runtime_kind_source") != "env" or obj.get("default_runtime_kind_available") is not True:
  print("unexpected builtin contract response", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("broker_url_default_configured") is not True or obj.get("broker_token_default_configured") is not True:
  print("expected broker defaults in builtin contract response", obj, file=sys.stderr)
  raise SystemExit(1)
if "not implemented" not in str(obj.get("builtin_unavailable_reason", "")):
  print("expected builtin unavailable reason", obj, file=sys.stderr)
  raise SystemExit(1)
if "not implemented" not in str(obj.get("error", "")):
  print("expected builtin not implemented error", obj, file=sys.stderr)
  raise SystemExit(1)
PY

external_resp_headers="${LOG_DIR}/voice_webrtc_peer_external_headers.txt"
external_resp_body="${LOG_DIR}/voice_webrtc_peer_external_body.json"
external_status="$(curl -sS --noproxy "*" --max-time 10 -o "${external_resp_body}" -D "${external_resp_headers}" -w '%{http_code}' \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${SESSION_DB_ID}\",\"action\":\"start\",\"runtime_kind\":\"external\",\"broker_session_id\":\"external-test-session\"}" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"
if [[ "${external_status}" != "500" ]]; then
  echo "expected explicit external runtime request to return 500 without configured tool, got ${external_status}" >&2
  cat "${external_resp_body}" >&2
  exit 1
fi
python3 - <<PY
import json, sys
obj = json.load(open(r'''${external_resp_body}''', 'r', encoding='utf-8'))
if obj.get("builtin_available") is not False or obj.get("bundled_available") is not True:
  print("unexpected external contract response", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("external_available") is not False or obj.get("default_runtime_kind") != "bundled":
  print("unexpected external contract response", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("default_runtime_kind_source") != "env" or obj.get("default_runtime_kind_available") is not True:
  print("unexpected external contract response", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("broker_url_default_configured") is not True or obj.get("broker_token_default_configured") is not True:
  print("expected broker defaults in external contract response", obj, file=sys.stderr)
  raise SystemExit(1)
if "not configured" not in str(obj.get("external_unavailable_reason", "")):
  print("expected external unavailable reason", obj, file=sys.stderr)
  raise SystemExit(1)
if "not configured" not in str(obj.get("error", "")):
  print("expected external not configured error", obj, file=sys.stderr)
  raise SystemExit(1)
PY

missing_broker_session_resp_body="${LOG_DIR}/voice_webrtc_peer_missing_broker_session_body.json"
MISSING_BROKER_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_missing_broker_$(date +%s)_$RANDOM"
curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${MISSING_BROKER_SESSION_ID}\"}" \
  "${DAEMON_URL}/api/v1/session/new" >/dev/null

missing_broker_session_status="$(curl -sS --noproxy "*" --max-time 10 -o "${missing_broker_session_resp_body}" -w '%{http_code}' \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${MISSING_BROKER_SESSION_ID}",
  "action": "start",
  "broker_session_id": "missing-broker-session",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 517
}))
PY
)" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"
if [[ "${missing_broker_session_status}" != "400" ]]; then
  echo "expected missing broker_session_id start to return 400, got ${missing_broker_session_status}" >&2
  cat "${missing_broker_session_resp_body}" >&2
  exit 1
fi
python3 - <<PY
import json, sys
obj = json.load(open(r'''${missing_broker_session_resp_body}''', 'r', encoding='utf-8'))
if obj.get("ok") is not False:
  print("expected missing broker_session_id start to fail", obj, file=sys.stderr)
  raise SystemExit(1)
if "broker_session_id not found" not in str(obj.get("error", "")):
  print("expected missing broker_session_id error", obj, file=sys.stderr)
  raise SystemExit(1)
PY

missing_broker_session_status_json="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer?session_id=$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${MISSING_BROKER_SESSION_ID}")")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${missing_broker_session_status_json}''')
if obj.get("running") is not False or obj.get("peer") is not None:
  print("expected no runtime after missing broker_session_id preflight failure", obj, file=sys.stderr)
  raise SystemExit(1)
PY

curl -fsS --noproxy "*" --max-time 10 -X DELETE \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  "${DAEMON_URL}/api/v1/session?session_id=$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${MISSING_BROKER_SESSION_ID}")" >/dev/null

CONFLICT_BROKER_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_conflict_broker_$(date +%s)_$RANDOM"
curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${CONFLICT_BROKER_SESSION_ID}\"}" \
  "${DAEMON_URL}/api/v1/session/new" >/dev/null

conflict_broker_session_resp_body="${LOG_DIR}/voice_webrtc_peer_conflict_broker_session_body.json"
conflict_broker_session_status="$(curl -sS --noproxy "*" --max-time 10 -o "${conflict_broker_session_resp_body}" -w '%{http_code}' \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${CONFLICT_BROKER_SESSION_ID}",
  "action": "start",
  "broker_session_id": "aud_conflict",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-conflict",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 518
}))
PY
)" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"
if [[ "${conflict_broker_session_status}" != "400" ]]; then
  echo "expected conflicting broker session start to return 400, got ${conflict_broker_session_status}" >&2
  cat "${conflict_broker_session_resp_body}" >&2
  exit 1
fi
python3 - <<PY
import json, sys
obj = json.load(open(r'''${conflict_broker_session_resp_body}''', 'r', encoding='utf-8'))
if obj.get("ok") is not False:
  print("expected conflicting broker session start to fail", obj, file=sys.stderr)
  raise SystemExit(1)
if "must be omitted" not in str(obj.get("error", "")):
  print("expected conflicting broker session validation error", obj, file=sys.stderr)
  raise SystemExit(1)
PY

conflict_broker_session_status_json="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer?session_id=$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${CONFLICT_BROKER_SESSION_ID}")")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${conflict_broker_session_status_json}''')
if obj.get("running") is not False or obj.get("peer") is not None:
  print("expected no runtime after conflicting broker session validation failure", obj, file=sys.stderr)
  raise SystemExit(1)
PY

curl -fsS --noproxy "*" --max-time 10 -X DELETE \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  "${DAEMON_URL}/api/v1/session?session_id=$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${CONFLICT_BROKER_SESSION_ID}")" >/dev/null

start_resp2="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${SESSION_DB_ID}",
  "action": "start",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-stop",
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
peer = obj.get("peer") or {}
if peer.get("runtime_kind") != "bundled":
  print("unexpected second peer runtime_kind", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("managed_broker_session") is not True or peer.get("broker_deployment_id") != "lab-stop":
  print("unexpected second managed broker session fields", obj, file=sys.stderr)
  raise SystemExit(1)
PY

BROKER_SESSION_ID2="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${start_resp2}''')
peer = obj.get("peer") or {}
sid = str(peer.get("broker_session_id") or "").strip()
if not sid:
  print("missing second broker_session_id in start response", file=sys.stderr)
  raise SystemExit(1)
print(sid)
PY
)"

VOICE_PEER_PID2="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${start_resp2}''')
peer = obj.get("peer") or {}
pid = peer.get("pid")
if not isinstance(pid, int) or pid <= 0:
  print("missing second pid in start response", file=sys.stderr)
  raise SystemExit(1)
print(pid)
PY
)"

wait_voice_peer_ready "${SESSION_DB_ID}" 1 status_json

kill -9 "${VOICE_PEER_PID2}"

session_exists_status="$(curl -sS --noproxy "*" --max-time 10 -o /dev/null -w '%{http_code}' \
  "http://127.0.0.1:${BROKER_PORT}/v1/audio/sessions/${BROKER_SESSION_ID2}" \
  -H "Authorization: Bearer audio-webui-token")"
if [[ "${session_exists_status}" != "200" ]]; then
  echo "expected broker audio session to still exist after peer SIGKILL, got ${session_exists_status}" >&2
  exit 1
fi

restart_agentd
wait_voice_peer_ready "${SESSION_DB_ID}" 0 stopped_json

python3 - <<PY
import json, sys
obj = json.loads(r'''${stopped_json}''')
peer = obj.get("peer") or {}
if peer.get("status_source") != "persisted":
  print("expected persisted stopped peer after forced-exit restart", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("running"):
  print("peer unexpectedly running after forced-exit restart", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("exit_signal") != 9:
  print("expected exit_signal 9 after SIGKILL", obj, file=sys.stderr)
  raise SystemExit(1)
PY

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
peer = obj.get("peer") or {}
if peer.get("runtime_kind") != "bundled":
  print("unexpected stopped peer runtime_kind", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("broker_session_deleted") is not True:
  print("expected broker_session_deleted cleanup result", obj, file=sys.stderr)
  raise SystemExit(1)
PY

wait_voice_peer_ready "${SESSION_DB_ID}" 0 stopped_json

python3 - <<PY
import json, sys
obj = json.loads(r'''${stopped_json}''')
peer = obj.get("peer") or {}
if peer.get("running"):
  print("peer still running after stop", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("status_source") != "persisted":
  print("expected persisted stopped peer after cleanup", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("exit_signal") != 9:
  print("expected exit_signal 9 to persist after cleanup", obj, file=sys.stderr)
  raise SystemExit(1)
PY

wait_broker_session_deleted "${BROKER_SESSION_ID2}"

start_resp3="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${SESSION_DB_ID}",
  "action": "start",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-delete",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 659
}))
PY
)" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${start_resp3}''')
if not obj.get("ok") or not obj.get("started"):
  print("voice_webrtc_peer third start failed", obj, file=sys.stderr)
  raise SystemExit(1)
peer = obj.get("peer") or {}
if peer.get("runtime_kind") != "bundled" or peer.get("managed_broker_session") is not True:
  print("unexpected third peer runtime state", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("broker_deployment_id") != "lab-delete":
  print("unexpected third broker deployment", obj, file=sys.stderr)
  raise SystemExit(1)
PY

BROKER_SESSION_ID3="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${start_resp3}''')
peer = obj.get("peer") or {}
sid = str(peer.get("broker_session_id") or "").strip()
if not sid:
  print("missing third broker_session_id in start response", file=sys.stderr)
  raise SystemExit(1)
print(sid)
PY
)"

VOICE_STDOUT_LOG3="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${start_resp3}''')
peer = obj.get("peer") or {}
path = str(peer.get("stdout_log_path") or "").strip()
if not path:
  print("missing third stdout_log_path in start response", file=sys.stderr)
  raise SystemExit(1)
print(path)
PY
)"

wait_voice_peer_ready "${SESSION_DB_ID}" 1 status_json

SESSION_DB_ID_Q="$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${SESSION_DB_ID}")"
delete_resp="$(curl -fsS --noproxy "*" --max-time 10 -X DELETE \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  "${DAEMON_URL}/api/v1/session?session_id=${SESSION_DB_ID_Q}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${delete_resp}''')
if not obj.get("ok") or obj.get("deleted_from_db") is not True:
  print("session delete with voice runtime cleanup failed", obj, file=sys.stderr)
  raise SystemExit(1)
cleanup = obj.get("voice_runtime_cleanup") or {}
if cleanup.get("runtime_present") is not True:
  print("expected voice runtime cleanup to find runtime", obj, file=sys.stderr)
  raise SystemExit(1)
if cleanup.get("runtime_was_running") is not True:
  print("expected running runtime before session delete", obj, file=sys.stderr)
  raise SystemExit(1)
if cleanup.get("broker_session_delete_attempted") is not True or cleanup.get("broker_session_deleted") is not True:
  print("expected broker session deletion during session delete", obj, file=sys.stderr)
  raise SystemExit(1)
if cleanup.get("persisted_record_cleared") is not True:
  print("expected persisted runtime cleanup during session delete", obj, file=sys.stderr)
  raise SystemExit(1)
PY

session_after_delete_body="${LOG_DIR}/voice_runtime_session_after_delete.json"
session_after_delete_status="$(curl -sS --noproxy "*" --max-time 10 \
  -o "${session_after_delete_body}" -w '%{http_code}' \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  "${DAEMON_URL}/api/v1/session?session_id=${SESSION_DB_ID_Q}")"
if [[ "${session_after_delete_status}" != "404" ]]; then
  echo "expected deleted session to return 404, got ${session_after_delete_status}" >&2
  cat "${session_after_delete_body}" >&2
  exit 1
fi

status_after_delete="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer?session_id=${SESSION_DB_ID_Q}")"

python3 - <<PY
import json, os, sys
obj = json.loads(r'''${status_after_delete}''')
if obj.get("session_exists") is not False:
  print("expected session_exists=false after delete", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("running") is not False:
  print("expected running=false after delete", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("peer") is not None:
  print("expected peer=null after delete cleanup", obj, file=sys.stderr)
  raise SystemExit(1)
if os.path.exists(r'''${VOICE_STDOUT_LOG3}'''):
  print("expected stdout log to be removed after session delete", r'''${VOICE_STDOUT_LOG3}''', file=sys.stderr)
  raise SystemExit(1)
PY

restart_agentd

status_after_delete_restart="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer?session_id=${SESSION_DB_ID_Q}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${status_after_delete_restart}''')
if obj.get("session_exists") is not False or obj.get("running") is not False or obj.get("peer") is not None:
  print("expected no resurrected peer state after restart", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("cleanup_on_missing_session") not in (None, {}):
  print("unexpected cleanup_on_missing_session after already-clean delete", obj, file=sys.stderr)
  raise SystemExit(1)
PY

wait_broker_session_deleted "${BROKER_SESSION_ID3}"

STALE_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_stale_$(date +%s)_$RANDOM"
curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${STALE_SESSION_ID}\"}" \
  "${DAEMON_URL}/api/v1/session/new" >/dev/null

start_resp4="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${STALE_SESSION_ID}",
  "action": "start",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-stale",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 784
}))
PY
)" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${start_resp4}''')
if not obj.get("ok") or not obj.get("started"):
  print("voice_webrtc_peer fourth start failed", obj, file=sys.stderr)
  raise SystemExit(1)
peer = obj.get("peer") or {}
if peer.get("runtime_kind") != "bundled" or peer.get("managed_broker_session") is not True:
  print("unexpected fourth peer runtime state", obj, file=sys.stderr)
  raise SystemExit(1)
PY

BROKER_SESSION_ID4="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${start_resp4}''')
peer = obj.get("peer") or {}
sid = str(peer.get("broker_session_id") or "").strip()
if not sid:
  print("missing fourth broker_session_id in start response", file=sys.stderr)
  raise SystemExit(1)
print(sid)
PY
)"

VOICE_STDOUT_LOG4="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${start_resp4}''')
peer = obj.get("peer") or {}
path = str(peer.get("stdout_log_path") or "").strip()
if not path:
  print("missing fourth stdout_log_path in start response", file=sys.stderr)
  raise SystemExit(1)
print(path)
PY
)"

wait_voice_peer_ready "${STALE_SESSION_ID}" 1 status_json

python3 - <<PY
import sqlite3
db_path = r'''${SESSION_DB_PATH}'''
sid = r'''${STALE_SESSION_ID}'''
conn = sqlite3.connect(db_path)
try:
    conn.execute("DELETE FROM sessions WHERE session_id = ?", (sid,))
    conn.commit()
finally:
    conn.close()
PY

STALE_SESSION_ID_Q="$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${STALE_SESSION_ID}")"
status_after_stale_delete="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer?session_id=${STALE_SESSION_ID_Q}")"

python3 - <<PY
import json, os, sys
obj = json.loads(r'''${status_after_stale_delete}''')
if obj.get("session_exists") is not False:
  print("expected session_exists=false after direct DB delete", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("running") is not False:
  print("expected running=false after stale cleanup", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("peer") is not None:
  print("expected peer=null after stale cleanup", obj, file=sys.stderr)
  raise SystemExit(1)
cleanup = obj.get("cleanup_on_missing_session") or {}
if cleanup.get("runtime_present") is not True or cleanup.get("runtime_was_running") is not True:
  print("expected cleanup_on_missing_session to report stale running runtime", obj, file=sys.stderr)
  raise SystemExit(1)
if cleanup.get("persisted_record_cleared") is not True:
  print("expected persisted runtime record cleared during stale cleanup", obj, file=sys.stderr)
  raise SystemExit(1)
if os.path.exists(r'''${VOICE_STDOUT_LOG4}'''):
  print("expected stale runtime stdout log removed", r'''${VOICE_STDOUT_LOG4}''', file=sys.stderr)
  raise SystemExit(1)
PY

wait_broker_session_deleted "${BROKER_SESSION_ID4}"

config_update_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "audio_webrtc": {
    "broker_url": "${VOICE_BROKER_URL}",
    "broker_token": "${VOICE_BROKER_TOKEN}",
    "peer_tool_path": "${PEER_TOOL}",
    "default_runtime_kind": "external",
    "node_bin": "node"
  }
}))
PY
)" \
  "${DAEMON_URL}/api/v1/config/update")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${config_update_resp}''')
audio = obj.get("audio_webrtc") or {}
if not obj.get("ok"):
  print("config update for audio_webrtc failed", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("broker_url_default_configured") is not True:
  print("expected updated broker_url_default_configured", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("broker_token_default_configured") is not True:
  print("expected updated broker_token_default_configured", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("peer_tool_path_configured") is not True:
  print("expected updated peer_tool_path_configured", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("default_runtime_kind") != "external" or audio.get("default_runtime_kind_source") != "config":
  print("expected updated default_runtime_kind=external", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("node_bin") != "node":
  print("expected updated node_bin=node", obj, file=sys.stderr)
  raise SystemExit(1)
PY

restart_agentd_without_voice_defaults

config_persisted_defaults="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  "${DAEMON_URL}/api/v1/config")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${config_persisted_defaults}''')
daemon = obj.get("daemon") or {}
audio = daemon.get("audio_webrtc") or {}
if audio.get("broker_url_default_configured") is not True:
  print("expected persisted broker_url_default_configured after restart", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("broker_token_default_configured") is not True:
  print("expected persisted broker_token_default_configured after restart", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("peer_tool_path_configured") is not True:
  print("expected persisted peer_tool_path_configured after restart", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("default_runtime_kind") != "external" or audio.get("default_runtime_kind_source") != "config":
  print("expected persisted default_runtime_kind=external after restart", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("node_bin") != "node":
  print("expected persisted node_bin=node after restart", obj, file=sys.stderr)
  raise SystemExit(1)
PY

CONFIG_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_cfg_$(date +%s)_$RANDOM"
curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${CONFIG_SESSION_ID}\"}" \
  "${DAEMON_URL}/api/v1/session/new" >/dev/null

config_start_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${CONFIG_SESSION_ID}",
  "action": "start",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-config",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 880
}))
PY
)" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${config_start_resp}''')
peer = obj.get("peer") or {}
if not obj.get("ok") or not obj.get("started"):
  print("voice_webrtc_peer config-backed start failed", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("broker_url_default_configured") is not True or obj.get("broker_token_default_configured") is not True:
  print("expected config-backed broker defaults", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("external_available") is not True or obj.get("default_runtime_kind") != "external":
  print("expected config-backed external default runtime", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("default_runtime_kind_source") != "config" or obj.get("default_runtime_kind_available") is not True:
  print("expected config-backed default runtime metadata", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("runtime_kind") != "external":
  print("expected config-backed start to resolve external runtime", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("managed_broker_session") is not True or peer.get("broker_deployment_id") != "lab-config":
  print("unexpected config-backed managed broker session fields", obj, file=sys.stderr)
  raise SystemExit(1)
PY

CONFIG_BROKER_SESSION_ID="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${config_start_resp}''')
peer = obj.get("peer") or {}
sid = str(peer.get("broker_session_id") or "").strip()
if not sid:
  print("missing config-backed broker_session_id", file=sys.stderr)
  raise SystemExit(1)
print(sid)
PY
)"

wait_voice_peer_ready "${CONFIG_SESSION_ID}" 1 status_json external external config 1

config_stop_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${CONFIG_SESSION_ID}\",\"action\":\"stop\"}" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${config_stop_resp}''')
if not obj.get("ok") or not obj.get("stopped"):
  print("voice_webrtc_peer config-backed stop failed", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("broker_session_deleted") is not True:
  print("expected broker_session_deleted on config-backed stop", obj, file=sys.stderr)
  raise SystemExit(1)
PY

wait_broker_session_deleted "${CONFIG_BROKER_SESSION_ID}"

CONFIG_SESSION_ID_Q="$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${CONFIG_SESSION_ID}")"
curl -fsS --noproxy "*" --max-time 10 -X DELETE \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  "${DAEMON_URL}/api/v1/session?session_id=${CONFIG_SESSION_ID_Q}" >/dev/null

EXTERNAL_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_external_$(date +%s)_$RANDOM"
curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${EXTERNAL_SESSION_ID}\"}" \
  "${DAEMON_URL}/api/v1/session/new" >/dev/null

external_config_start_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${EXTERNAL_SESSION_ID}",
  "action": "start",
  "runtime_kind": "external",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-external-config",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 901
}))
PY
)" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${external_config_start_resp}''')
peer = obj.get("peer") or {}
if not obj.get("ok") or not obj.get("started"):
  print("voice_webrtc_peer external config-backed start failed", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("runtime_kind") != "external":
  print("expected external runtime_kind", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("managed_broker_session") is not True or peer.get("broker_deployment_id") != "lab-external-config":
  print("unexpected external managed broker session fields", obj, file=sys.stderr)
  raise SystemExit(1)
PY

EXTERNAL_BROKER_SESSION_ID="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${external_config_start_resp}''')
peer = obj.get("peer") or {}
sid = str(peer.get("broker_session_id") or "").strip()
if not sid:
  print("missing external broker_session_id", file=sys.stderr)
  raise SystemExit(1)
print(sid)
PY
)"

wait_voice_peer_ready "${EXTERNAL_SESSION_ID}" 1 status_json external external config 1

external_config_stop_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${EXTERNAL_SESSION_ID}\",\"action\":\"stop\",\"runtime_kind\":\"builtin\"}" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${external_config_stop_resp}''')
peer = obj.get("peer") or {}
if not obj.get("ok") or not obj.get("stopped"):
  print("voice_webrtc_peer external config-backed stop failed", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("runtime_kind") != "external":
  print("expected external runtime_kind during stop", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("broker_session_deleted") is not True:
  print("expected broker_session_deleted on external config-backed stop", obj, file=sys.stderr)
  raise SystemExit(1)
PY

wait_broker_session_deleted "${EXTERNAL_BROKER_SESSION_ID}"

invalid_broker_defaults_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "audio_webrtc": {
    "broker_url": "${VOICE_BROKER_URL}",
    "broker_token": "bad\\nvoice\\ntoken",
    "peer_tool_path": "${PEER_TOOL}",
    "default_runtime_kind": "external",
    "node_bin": "node"
  }
}))
PY
)" \
  "${DAEMON_URL}/api/v1/config/update")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${invalid_broker_defaults_resp}''')
if not obj.get("ok"):
  print("failed to set invalid broker token defaults for lazy-validation proof", obj, file=sys.stderr)
  raise SystemExit(1)
PY

MANAGED_BAD_STOP_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_managed_bad_stop_$(date +%s)_$RANDOM"
curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${MANAGED_BAD_STOP_SESSION_ID}\"}" \
  "${DAEMON_URL}/api/v1/session/new" >/dev/null

managed_bad_stop_start_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${MANAGED_BAD_STOP_SESSION_ID}",
  "action": "start",
  "runtime_kind": "external",
  "broker_url": "${VOICE_BROKER_URL}",
  "broker_token": "${VOICE_BROKER_TOKEN}",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-managed-bad-stop",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 779
}))
PY
)" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${managed_bad_stop_start_resp}''')
peer = obj.get("peer") or {}
if not obj.get("ok") or not obj.get("started"):
  print("managed bad-stop runtime start failed", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("managed_broker_session") is not True:
  print("expected managed bad-stop runtime to own broker session", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("runtime_kind") != "external":
  print("expected managed bad-stop runtime_kind=external", obj, file=sys.stderr)
  raise SystemExit(1)
PY

MANAGED_BAD_STOP_BROKER_SESSION_ID="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${managed_bad_stop_start_resp}''')
peer = obj.get("peer") or {}
sid = str(peer.get("broker_session_id") or "").strip()
if not sid:
  print("missing managed bad-stop broker_session_id", file=sys.stderr)
  raise SystemExit(1)
print(sid)
PY
)"

wait_voice_peer_ready "${MANAGED_BAD_STOP_SESSION_ID}" 1 status_json external external config 1

managed_bad_stop_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${MANAGED_BAD_STOP_SESSION_ID}\",\"action\":\"stop\"}" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${managed_bad_stop_resp}''')
peer = obj.get("peer") or {}
if not obj.get("ok") or not obj.get("stopped"):
  print("managed runtime stop should still succeed when broker deletion fails", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("broker_session_deleted") is not False:
  print("expected managed bad-stop broker_session_deleted=false", obj, file=sys.stderr)
  raise SystemExit(1)
if "invalid configured audio_webrtc_broker_token" not in str(obj.get("broker_session_delete_error", "")):
  print("expected managed bad-stop broker_session_delete_error", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("managed_broker_session") is not True or peer.get("running"):
  print("expected managed bad-stop peer to be stopped locally", obj, file=sys.stderr)
  raise SystemExit(1)
PY

managed_bad_stop_broker_status="$(curl -sS --noproxy "*" --max-time 10 -o /dev/null -w '%{http_code}' \
  "http://127.0.0.1:${BROKER_PORT}/v1/audio/sessions/${MANAGED_BAD_STOP_BROKER_SESSION_ID}" \
  -H "Authorization: Bearer audio-webui-token")"
if [[ "${managed_bad_stop_broker_status}" != "200" && "${managed_bad_stop_broker_status}" != "404" ]]; then
  echo "expected managed bad-stop broker session inspect to return 200 or 404, got ${managed_bad_stop_broker_status}" >&2
  exit 1
fi

managed_bad_stop_delete_status="$(curl -sS --noproxy "*" --max-time 10 -o /dev/null -w '%{http_code}' -X DELETE \
  "http://127.0.0.1:${BROKER_PORT}/v1/audio/sessions/${MANAGED_BAD_STOP_BROKER_SESSION_ID}" \
  -H "Authorization: Bearer audio-webui-token")"
if [[ "${managed_bad_stop_delete_status}" != "200" && "${managed_bad_stop_delete_status}" != "404" ]]; then
  echo "expected managed bad-stop broker session delete to return 200 or 404, got ${managed_bad_stop_delete_status}" >&2
  exit 1
fi

MANAGED_BAD_DELETE_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_managed_bad_delete_$(date +%s)_$RANDOM"
curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${MANAGED_BAD_DELETE_SESSION_ID}\"}" \
  "${DAEMON_URL}/api/v1/session/new" >/dev/null

managed_bad_delete_start_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${MANAGED_BAD_DELETE_SESSION_ID}",
  "action": "start",
  "runtime_kind": "external",
  "broker_url": "${VOICE_BROKER_URL}",
  "broker_token": "${VOICE_BROKER_TOKEN}",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-managed-bad-delete",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 780
}))
PY
)" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${managed_bad_delete_start_resp}''')
peer = obj.get("peer") or {}
if not obj.get("ok") or not obj.get("started"):
  print("managed bad-delete runtime start failed", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("managed_broker_session") is not True:
  print("expected managed bad-delete runtime to own broker session", obj, file=sys.stderr)
  raise SystemExit(1)
PY

MANAGED_BAD_DELETE_BROKER_SESSION_ID="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${managed_bad_delete_start_resp}''')
peer = obj.get("peer") or {}
sid = str(peer.get("broker_session_id") or "").strip()
if not sid:
  print("missing managed bad-delete broker_session_id", file=sys.stderr)
  raise SystemExit(1)
print(sid)
PY
)"

wait_voice_peer_ready "${MANAGED_BAD_DELETE_SESSION_ID}" 1 status_json external external config 1

MANAGED_BAD_DELETE_SESSION_ID_Q="$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${MANAGED_BAD_DELETE_SESSION_ID}")"
managed_bad_delete_resp="$(curl -fsS --noproxy "*" --max-time 10 -X DELETE \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  "${DAEMON_URL}/api/v1/session?session_id=${MANAGED_BAD_DELETE_SESSION_ID_Q}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${managed_bad_delete_resp}''')
cleanup = obj.get("voice_runtime_cleanup") or {}
peer = cleanup.get("peer") or {}
if not obj.get("ok") or obj.get("deleted_from_db") is not True:
  print("managed delete should still succeed when broker deletion fails", obj, file=sys.stderr)
  raise SystemExit(1)
if cleanup.get("runtime_present") is not True or cleanup.get("stopped") is not True:
  print("expected managed bad-delete runtime cleanup summary", obj, file=sys.stderr)
  raise SystemExit(1)
if cleanup.get("broker_session_delete_attempted") is not True:
  print("expected managed bad-delete broker_session_delete_attempted=true", obj, file=sys.stderr)
  raise SystemExit(1)
if cleanup.get("broker_session_deleted") is not False:
  print("expected managed bad-delete broker_session_deleted=false", obj, file=sys.stderr)
  raise SystemExit(1)
if "invalid configured audio_webrtc_broker_token" not in str(cleanup.get("broker_session_delete_error", "")):
  print("expected managed bad-delete broker_session_delete_error", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("managed_broker_session") is not True:
  print("expected managed bad-delete cleanup peer managed_broker_session=true", obj, file=sys.stderr)
  raise SystemExit(1)
PY

managed_bad_delete_broker_status="$(curl -sS --noproxy "*" --max-time 10 -o /dev/null -w '%{http_code}' \
  "http://127.0.0.1:${BROKER_PORT}/v1/audio/sessions/${MANAGED_BAD_DELETE_BROKER_SESSION_ID}" \
  -H "Authorization: Bearer audio-webui-token")"
if [[ "${managed_bad_delete_broker_status}" != "200" && "${managed_bad_delete_broker_status}" != "404" ]]; then
  echo "expected managed bad-delete broker session inspect to return 200 or 404, got ${managed_bad_delete_broker_status}" >&2
  exit 1
fi

managed_bad_delete_delete_status="$(curl -sS --noproxy "*" --max-time 10 -o /dev/null -w '%{http_code}' -X DELETE \
  "http://127.0.0.1:${BROKER_PORT}/v1/audio/sessions/${MANAGED_BAD_DELETE_BROKER_SESSION_ID}" \
  -H "Authorization: Bearer audio-webui-token")"
if [[ "${managed_bad_delete_delete_status}" != "200" && "${managed_bad_delete_delete_status}" != "404" ]]; then
  echo "expected managed bad-delete broker session delete to return 200 or 404, got ${managed_bad_delete_delete_status}" >&2
  exit 1
fi

BORROWED_STOP_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_borrowed_stop_$(date +%s)_$RANDOM"
curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${BORROWED_STOP_SESSION_ID}\"}" \
  "${DAEMON_URL}/api/v1/session/new" >/dev/null

borrowed_stop_broker_create_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  "http://127.0.0.1:${BROKER_PORT}/v1/audio/sessions" \
  -H "Authorization: Bearer audio-webui-token" \
  -H 'Content-Type: application/json' \
  -d '{"agent_id":"a-1","mode":"webrtc"}')"

BORROWED_STOP_BROKER_SESSION_ID="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${borrowed_stop_broker_create_resp}''')
sid = str(obj.get("session_id") or "").strip()
if not obj.get("ok") or not sid:
  print("failed to create borrowed stop broker session", obj, file=sys.stderr)
  raise SystemExit(1)
print(sid)
PY
)"

borrowed_stop_start_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${BORROWED_STOP_SESSION_ID}",
  "action": "start",
  "runtime_kind": "external",
  "broker_session_id": "${BORROWED_STOP_BROKER_SESSION_ID}",
  "broker_url": "${VOICE_BROKER_URL}",
  "broker_token": "${VOICE_BROKER_TOKEN}",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 777
}))
PY
)" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${borrowed_stop_start_resp}''')
peer = obj.get("peer") or {}
if not obj.get("ok") or not obj.get("started"):
  print("borrowed stop runtime start failed", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("managed_broker_session") is not False:
  print("expected borrowed stop runtime to keep broker ownership external", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("runtime_kind") != "external":
  print("expected borrowed stop runtime_kind=external", obj, file=sys.stderr)
  raise SystemExit(1)
PY

wait_voice_peer_ready "${BORROWED_STOP_SESSION_ID}" 1 status_json external external config 1

borrowed_stop_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${BORROWED_STOP_SESSION_ID}\",\"action\":\"stop\"}" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${borrowed_stop_resp}''')
peer = obj.get("peer") or {}
if not obj.get("ok") or not obj.get("stopped"):
  print("borrowed runtime stop should ignore invalid configured broker token", obj, file=sys.stderr)
  raise SystemExit(1)
if "broker_session_deleted" in obj:
  print("borrowed runtime stop should not attempt broker deletion", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("managed_broker_session") is not False:
  print("expected borrowed runtime stop to keep managed_broker_session=false", obj, file=sys.stderr)
  raise SystemExit(1)
PY

borrowed_stop_delete_status="$(curl -sS --noproxy "*" --max-time 10 -o /dev/null -w '%{http_code}' -X DELETE \
  "http://127.0.0.1:${BROKER_PORT}/v1/audio/sessions/${BORROWED_STOP_BROKER_SESSION_ID}" \
  -H "Authorization: Bearer audio-webui-token")"
if [[ "${borrowed_stop_delete_status}" != "200" && "${borrowed_stop_delete_status}" != "404" ]]; then
  echo "expected borrowed stop broker session delete to return 200 or 404, got ${borrowed_stop_delete_status}" >&2
  exit 1
fi

BORROWED_DELETE_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_borrowed_delete_$(date +%s)_$RANDOM"
curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${BORROWED_DELETE_SESSION_ID}\"}" \
  "${DAEMON_URL}/api/v1/session/new" >/dev/null

borrowed_delete_broker_create_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  "http://127.0.0.1:${BROKER_PORT}/v1/audio/sessions" \
  -H "Authorization: Bearer audio-webui-token" \
  -H 'Content-Type: application/json' \
  -d '{"agent_id":"a-1","mode":"webrtc"}')"

BORROWED_DELETE_BROKER_SESSION_ID="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${borrowed_delete_broker_create_resp}''')
sid = str(obj.get("session_id") or "").strip()
if not obj.get("ok") or not sid:
  print("failed to create borrowed delete broker session", obj, file=sys.stderr)
  raise SystemExit(1)
print(sid)
PY
)"

borrowed_delete_start_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${BORROWED_DELETE_SESSION_ID}",
  "action": "start",
  "runtime_kind": "external",
  "broker_session_id": "${BORROWED_DELETE_BROKER_SESSION_ID}",
  "broker_url": "${VOICE_BROKER_URL}",
  "broker_token": "${VOICE_BROKER_TOKEN}",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 778
}))
PY
)" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${borrowed_delete_start_resp}''')
peer = obj.get("peer") or {}
if not obj.get("ok") or not obj.get("started"):
  print("borrowed delete runtime start failed", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("managed_broker_session") is not False:
  print("expected borrowed delete runtime to keep broker ownership external", obj, file=sys.stderr)
  raise SystemExit(1)
PY

wait_voice_peer_ready "${BORROWED_DELETE_SESSION_ID}" 1 status_json external external config 1

BORROWED_DELETE_SESSION_ID_Q="$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${BORROWED_DELETE_SESSION_ID}")"
borrowed_delete_resp="$(curl -fsS --noproxy "*" --max-time 10 -X DELETE \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  "${DAEMON_URL}/api/v1/session?session_id=${BORROWED_DELETE_SESSION_ID_Q}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${borrowed_delete_resp}''')
cleanup = obj.get("voice_runtime_cleanup") or {}
peer = cleanup.get("peer") or {}
if not obj.get("ok"):
  print("borrowed delete cleanup should ignore invalid configured broker token", obj, file=sys.stderr)
  raise SystemExit(1)
if cleanup.get("runtime_present") is not True or cleanup.get("stopped") is not True:
  print("expected borrowed delete runtime cleanup summary", obj, file=sys.stderr)
  raise SystemExit(1)
if cleanup.get("broker_session_delete_attempted") is not False:
  print("borrowed delete cleanup should not attempt broker deletion", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("managed_broker_session") is not False:
  print("expected borrowed delete cleanup peer managed_broker_session=false", obj, file=sys.stderr)
  raise SystemExit(1)
PY

borrowed_delete_broker_delete_status="$(curl -sS --noproxy "*" --max-time 10 -o /dev/null -w '%{http_code}' -X DELETE \
  "http://127.0.0.1:${BROKER_PORT}/v1/audio/sessions/${BORROWED_DELETE_BROKER_SESSION_ID}" \
  -H "Authorization: Bearer audio-webui-token")"
if [[ "${borrowed_delete_broker_delete_status}" != "200" && "${borrowed_delete_broker_delete_status}" != "404" ]]; then
  echo "expected borrowed delete broker session delete to return 200 or 404, got ${borrowed_delete_broker_delete_status}" >&2
  exit 1
fi

restore_broker_defaults_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "audio_webrtc": {
    "broker_url": "${VOICE_BROKER_URL}",
    "broker_token": "${VOICE_BROKER_TOKEN}",
    "peer_tool_path": "${PEER_TOOL}",
    "default_runtime_kind": "external",
    "node_bin": "node"
  }
}))
PY
)" \
  "${DAEMON_URL}/api/v1/config/update")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${restore_broker_defaults_resp}''')
if not obj.get("ok"):
  print("failed to restore broker defaults after lazy-validation proof", obj, file=sys.stderr)
  raise SystemExit(1)
PY

NOOP_STOP_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_noop_stop_$(date +%s)_$RANDOM"
curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${NOOP_STOP_SESSION_ID}\"}" \
  "${DAEMON_URL}/api/v1/session/new" >/dev/null

noop_stop_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${NOOP_STOP_SESSION_ID}\",\"action\":\"stop\",\"runtime_kind\":\"not-a-real-runtime-kind\"}" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${noop_stop_resp}''')
if not obj.get("ok"):
  print("expected noop stop with ignored runtime_kind to succeed", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("stopped") is not False or obj.get("reason") != "not_running":
  print("expected noop stop result", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("peer") is not None:
  print("expected noop stop peer=null", obj, file=sys.stderr)
  raise SystemExit(1)
PY

NOOP_STOP_SESSION_ID_Q="$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${NOOP_STOP_SESSION_ID}")"
curl -fsS --noproxy "*" --max-time 10 -X DELETE \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  "${DAEMON_URL}/api/v1/session?session_id=${NOOP_STOP_SESSION_ID_Q}" >/dev/null

EXTERNAL_SESSION_ID_Q="$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${EXTERNAL_SESSION_ID}")"
curl -fsS --noproxy "*" --max-time 10 -X DELETE \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  "${DAEMON_URL}/api/v1/session?session_id=${EXTERNAL_SESSION_ID_Q}" >/dev/null

unavailable_default_config_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "audio_webrtc": {
    "peer_tool_path": None,
    "default_runtime_kind": "external",
    "node_bin": "node"
  }
}))
PY
)" \
  "${DAEMON_URL}/api/v1/config/update")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${unavailable_default_config_resp}''')
audio = obj.get("audio_webrtc") or {}
if not obj.get("ok"):
  print("config update for unavailable external default failed", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("peer_tool_path_configured") is not False:
  print("expected cleared peer_tool_path_configured", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("builtin_available") is not False or audio.get("bundled_available") is not True:
  print("unexpected bundled/builtin availability after clearing external tool", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("external_available") is not False:
  print("expected external_available=false after clearing external tool", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("default_runtime_kind") != "external" or audio.get("default_runtime_kind_source") != "config":
  print("expected config-backed external default to remain selected", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("default_runtime_kind_available") is not False:
  print("expected config-backed external default to be unavailable", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("node_bin") != "node":
  print("expected node_bin=node after clearing external tool", obj, file=sys.stderr)
  raise SystemExit(1)
PY

restart_agentd_without_voice_defaults

unavailable_default_config_get="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  "${DAEMON_URL}/api/v1/config")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${unavailable_default_config_get}''')
daemon = obj.get("daemon") or {}
audio = daemon.get("audio_webrtc") or {}
if audio.get("peer_tool_path_configured") is not False:
  print("expected persisted cleared peer_tool_path_configured", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("builtin_available") is not False or audio.get("bundled_available") is not True:
  print("unexpected bundled/builtin availability after restart", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("external_available") is not False:
  print("expected persisted external_available=false after restart", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("default_runtime_kind") != "external" or audio.get("default_runtime_kind_source") != "config":
  print("expected persisted external default after restart", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("default_runtime_kind_available") is not False:
  print("expected persisted default_runtime_kind_available=false after restart", obj, file=sys.stderr)
  raise SystemExit(1)
PY

UNAVAILABLE_DEFAULT_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_default_unavailable_$(date +%s)_$RANDOM"
curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${UNAVAILABLE_DEFAULT_SESSION_ID}\"}" \
  "${DAEMON_URL}/api/v1/session/new" >/dev/null

set +e
unavailable_default_start_resp="$(curl -sS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${UNAVAILABLE_DEFAULT_SESSION_ID}",
  "action": "start",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-default-unavailable",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 903
}))
PY
)" \
  -w $'\n%{http_code}' \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"
unavailable_default_start_curl_rc=$?
set -e
if [[ ${unavailable_default_start_curl_rc} -ne 0 ]]; then
  echo "voice_webrtc_peer unavailable-default start request failed to complete" >&2
  exit 1
fi

unavailable_default_start_code="$(printf '%s' "${unavailable_default_start_resp}" | tail -n 1)"
unavailable_default_start_body="$(printf '%s' "${unavailable_default_start_resp}" | sed '$d')"
if [[ "${unavailable_default_start_code}" != "500" ]]; then
  echo "expected unavailable external default start to return http 500, got ${unavailable_default_start_code}" >&2
  exit 1
fi

python3 - <<PY
import json, sys
obj = json.loads(r'''${unavailable_default_start_body}''')
if obj.get("ok") is not False:
  print("expected unavailable external default start to fail", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("builtin_available") is not False or obj.get("bundled_available") is not True:
  print("unexpected bundled/builtin availability on unavailable default start", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("external_available") is not False:
  print("expected external_available=false on unavailable default start", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("default_runtime_kind") != "external" or obj.get("default_runtime_kind_source") != "config":
  print("expected config-backed external default on unavailable default start", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("default_runtime_kind_available") is not False:
  print("expected unavailable default_runtime_kind_available=false on start failure", obj, file=sys.stderr)
  raise SystemExit(1)
if "audio_webrtc_peer_tool_path not configured" not in str(obj.get("error") or ""):
  print("expected missing peer tool path error on unavailable default start", obj, file=sys.stderr)
  raise SystemExit(1)
PY

UNAVAILABLE_DEFAULT_SESSION_ID_Q="$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${UNAVAILABLE_DEFAULT_SESSION_ID}")"
curl -fsS --noproxy "*" --max-time 10 -X DELETE \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  "${DAEMON_URL}/api/v1/session?session_id=${UNAVAILABLE_DEFAULT_SESSION_ID_Q}" >/dev/null

restored_external_default_config_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "audio_webrtc": {
    "peer_tool_path": "${PEER_TOOL}",
    "default_runtime_kind": "external",
    "node_bin": "node"
  }
}))
PY
)" \
  "${DAEMON_URL}/api/v1/config/update")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${restored_external_default_config_resp}''')
audio = obj.get("audio_webrtc") or {}
if not obj.get("ok"):
  print("failed to restore external default config after unavailable-default test", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("peer_tool_path_configured") is not True:
  print("expected restored peer_tool_path_configured", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("external_available") is not True or audio.get("default_runtime_kind_available") is not True:
  print("expected restored external availability after unavailable-default test", obj, file=sys.stderr)
  raise SystemExit(1)
PY

python3 - <<PY
import json, sqlite3
db_path = r'''${SESSION_DB_PATH}'''
conn = sqlite3.connect(db_path)
try:
    row = conn.execute("SELECT value FROM meta WHERE key = 'daemon.runtime_config_json'").fetchone()
    if row is None:
        raise SystemExit("missing daemon.runtime_config_json before corruption test")
    raw = row[0]
    data = json.loads(raw)
    audio = data.get("audio_webrtc") or {}
    audio["default_runtime_kind"] = "not-a-real-runtime-kind"
    data["audio_webrtc"] = audio
    conn.execute("UPDATE meta SET value = ? WHERE key = 'daemon.runtime_config_json'", (json.dumps(data),))
    conn.commit()
finally:
    conn.close()
PY

restart_agentd_without_voice_defaults

invalid_default_self_healed="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  "${DAEMON_URL}/api/v1/config")"

python3 - <<PY
import json, sqlite3, sys
obj = json.loads(r'''${invalid_default_self_healed}''')
daemon = obj.get("daemon") or {}
audio = daemon.get("audio_webrtc") or {}
if audio.get("default_runtime_kind") is not None or audio.get("default_runtime_kind_source") != "auto":
  print("expected invalid persisted default_runtime_kind to self-heal to auto", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("peer_tool_path_configured") is not True:
  print("expected external seam to remain configured after self-heal", obj, file=sys.stderr)
  raise SystemExit(1)
db_path = r'''${SESSION_DB_PATH}'''
conn = sqlite3.connect(db_path)
try:
    row = conn.execute("SELECT value FROM meta WHERE key = 'daemon.runtime_config_json'").fetchone()
    if row is None:
        print("missing daemon.runtime_config_json after self-heal", file=sys.stderr)
        raise SystemExit(1)
    data = json.loads(row[0])
    audio_cfg = data.get("audio_webrtc") or {}
    if audio_cfg.get("default_runtime_kind", "__missing__") is not None:
        print("expected persisted invalid default_runtime_kind to be rewritten to null", data, file=sys.stderr)
        raise SystemExit(1)
finally:
    conn.close()
PY

SELF_HEAL_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_self_heal_$(date +%s)_$RANDOM"
curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${SELF_HEAL_SESSION_ID}\"}" \
  "${DAEMON_URL}/api/v1/session/new" >/dev/null

self_heal_start_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${SELF_HEAL_SESSION_ID}",
  "action": "start",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-self-heal",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 902
}))
PY
)" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${self_heal_start_resp}''')
peer = obj.get("peer") or {}
if not obj.get("ok") or not obj.get("started"):
  print("voice_webrtc_peer self-heal fallback start failed", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("default_runtime_kind") != "bundled" or obj.get("default_runtime_kind_source") != "auto":
  print("expected auto/bundled default after self-heal fallback", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("runtime_kind") != "bundled":
  print("expected bundled runtime after self-heal fallback", obj, file=sys.stderr)
  raise SystemExit(1)
PY

SELF_HEAL_BROKER_SESSION_ID="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${self_heal_start_resp}''')
peer = obj.get("peer") or {}
sid = str(peer.get("broker_session_id") or "").strip()
if not sid:
  print("missing self-heal broker_session_id", file=sys.stderr)
  raise SystemExit(1)
print(sid)
PY
)"

wait_voice_peer_ready "${SELF_HEAL_SESSION_ID}" 1 status_json bundled bundled auto 1

self_heal_stop_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${SELF_HEAL_SESSION_ID}\",\"action\":\"stop\"}" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${self_heal_stop_resp}''')
if not obj.get("ok") or not obj.get("stopped"):
  print("voice_webrtc_peer self-heal fallback stop failed", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("broker_session_deleted") is not True:
  print("expected broker_session_deleted after self-heal fallback stop", obj, file=sys.stderr)
  raise SystemExit(1)
PY

wait_broker_session_deleted "${SELF_HEAL_BROKER_SESSION_ID}"

SELF_HEAL_SESSION_ID_Q="$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${SELF_HEAL_SESSION_ID}")"
curl -fsS --noproxy "*" --max-time 10 -X DELETE \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  "${DAEMON_URL}/api/v1/session?session_id=${SELF_HEAL_SESSION_ID_Q}" >/dev/null

config_invalid_node_bin_update_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "audio_webrtc": {
    "node_bin": "definitely-not-a-real-node-binary"
  }
}))
PY
)" \
  "${DAEMON_URL}/api/v1/config/update")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${config_invalid_node_bin_update_resp}''')
audio = obj.get("audio_webrtc") or {}
if not obj.get("ok"):
  print("config update for invalid node_bin failed", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("node_bin") != "definitely-not-a-real-node-binary":
  print("expected persisted invalid node_bin", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("bundled_available") is not False or audio.get("external_available") is not False:
  print("expected bundled/external unavailable with missing node_bin", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("default_runtime_kind_available") is not False:
  print("expected default_runtime_kind_available=false with missing node_bin", obj, file=sys.stderr)
  raise SystemExit(1)
if "not found" not in str(audio.get("bundled_unavailable_reason", "")):
  print("expected bundled unavailable reason for missing node_bin", obj, file=sys.stderr)
  raise SystemExit(1)
if "not found" not in str(audio.get("external_unavailable_reason", "")):
  print("expected external unavailable reason for missing node_bin", obj, file=sys.stderr)
  raise SystemExit(1)
PY

restart_agentd_without_voice_defaults

invalid_node_bin_config_json="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  "${DAEMON_URL}/api/v1/config")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${invalid_node_bin_config_json}''')
daemon = obj.get("daemon") or {}
audio = daemon.get("audio_webrtc") or {}
if audio.get("default_runtime_kind") is not None or audio.get("default_runtime_kind_source") != "auto":
  print("expected auto default runtime policy with invalid node_bin", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("bundled_available") is not False or audio.get("external_available") is not False:
  print("expected bundled/external unavailable after restart with invalid node_bin", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("default_runtime_kind_available") is not False:
  print("expected default runtime unavailable after restart with invalid node_bin", obj, file=sys.stderr)
  raise SystemExit(1)
if "not found" not in str(audio.get("default_runtime_kind_unavailable_reason", "")):
  print("expected default unavailable reason for missing node_bin", obj, file=sys.stderr)
  raise SystemExit(1)
PY

INVALID_NODE_BIN_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_invalid_node_bin_$(date +%s)_$RANDOM"
curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${INVALID_NODE_BIN_SESSION_ID}\"}" \
  "${DAEMON_URL}/api/v1/session/new" >/dev/null

invalid_node_bin_start_body="${LOG_DIR}/voice_webrtc_peer_invalid_node_bin_start_body.json"
invalid_node_bin_start_status="$(curl -sS --noproxy "*" --max-time 10 -o "${invalid_node_bin_start_body}" -w '%{http_code}' \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${INVALID_NODE_BIN_SESSION_ID}",
  "action": "start",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-invalid-node-bin",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 932,
  "startup_wait_ms": 1000
}))
PY
)" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"
if [[ "${invalid_node_bin_start_status}" != "500" ]]; then
  echo "expected invalid node_bin start response to return 500, got ${invalid_node_bin_start_status}" >&2
  cat "${invalid_node_bin_start_body}" >&2
  exit 1
fi

python3 - <<PY
import json, sys
obj = json.load(open(r'''${invalid_node_bin_start_body}''', 'r', encoding='utf-8'))
if obj.get("default_runtime_kind_available") is not False:
  print("expected unavailable default runtime on invalid node_bin start", obj, file=sys.stderr)
  raise SystemExit(1)
if "audio_webrtc_peer_node_bin not found" not in str(obj.get("error", "")):
  print("expected direct invalid node_bin error", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("startup_cleanup") is not None:
  print("expected no startup cleanup for preflight node_bin failure", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("peer") is not None:
  print("expected no peer state for preflight node_bin failure", obj, file=sys.stderr)
  raise SystemExit(1)
if "not found" not in str(obj.get("default_runtime_kind_unavailable_reason", "")):
  print("expected default unavailable reason on invalid node_bin start", obj, file=sys.stderr)
  raise SystemExit(1)
PY

INVALID_NODE_BIN_SESSION_ID_Q="$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${INVALID_NODE_BIN_SESSION_ID}")"
invalid_node_bin_status_json="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer?session_id=${INVALID_NODE_BIN_SESSION_ID_Q}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${invalid_node_bin_status_json}''')
if obj.get("session_exists") is not True:
  print("expected session row to remain after invalid node_bin preflight failure", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("running") is not False or obj.get("peer") is not None:
  print("expected no runtime after invalid node_bin preflight failure", obj, file=sys.stderr)
  raise SystemExit(1)
PY

curl -fsS --noproxy "*" --max-time 10 -X DELETE \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  "${DAEMON_URL}/api/v1/session?session_id=${INVALID_NODE_BIN_SESSION_ID_Q}" >/dev/null

config_failfast_update_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "audio_webrtc": {
    "node_bin": "false"
  }
}))
PY
)" \
  "${DAEMON_URL}/api/v1/config/update")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${config_failfast_update_resp}''')
audio = obj.get("audio_webrtc") or {}
if not obj.get("ok"):
  print("config update for fail-fast node_bin failed", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("node_bin") != "false":
  print("expected persisted fail-fast node_bin", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("bundled_available") is not True or audio.get("default_runtime_kind_available") is not True:
  print("expected launchable backend availability for fail-fast runtime test", obj, file=sys.stderr)
  raise SystemExit(1)
PY

restart_agentd_without_voice_defaults

FAIL_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_fail_$(date +%s)_$RANDOM"
curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${FAIL_SESSION_ID}\"}" \
  "${DAEMON_URL}/api/v1/session/new" >/dev/null

fail_start_body="${LOG_DIR}/voice_webrtc_peer_fail_start_body.json"
fail_start_status="$(curl -sS --noproxy "*" --max-time 10 -o "${fail_start_body}" -w '%{http_code}' \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${FAIL_SESSION_ID}",
  "action": "start",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-fail-fast",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 932,
  "startup_wait_ms": 1000
}))
PY
)" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"
if [[ "${fail_start_status}" != "500" ]]; then
  echo "expected fail-fast startup response to return 500, got ${fail_start_status}" >&2
  cat "${fail_start_body}" >&2
  exit 1
fi

python3 - <<PY
import json, sys
obj = json.load(open(r'''${fail_start_body}''', 'r', encoding='utf-8'))
cleanup = obj.get("startup_cleanup") or {}
peer = obj.get("peer") or {}
if obj.get("startup_confirmed") is not False:
  print("expected startup_confirmed=false for fail-fast start", obj, file=sys.stderr)
  raise SystemExit(1)
if cleanup.get("runtime_present") is not True:
  print("expected startup cleanup to observe runtime", obj, file=sys.stderr)
  raise SystemExit(1)
if cleanup.get("broker_session_delete_attempted") is not True or cleanup.get("broker_session_deleted") is not True:
  print("expected startup cleanup to delete managed broker session", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("exit_code") != 1:
  print("expected fail-fast peer exit_code 1", obj, file=sys.stderr)
  raise SystemExit(1)
if "exited before ready" not in str(obj.get("error", "")):
  print("expected fail-fast startup error", obj, file=sys.stderr)
  raise SystemExit(1)
PY

FAIL_SESSION_ID_Q="$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${FAIL_SESSION_ID}")"
fail_status_json="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer?session_id=${FAIL_SESSION_ID_Q}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${fail_status_json}''')
if obj.get("session_exists") is not True:
  print("expected session row to remain after fail-fast startup", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("running") is not False or obj.get("peer") is not None:
  print("expected no surviving runtime after fail-fast startup cleanup", obj, file=sys.stderr)
  raise SystemExit(1)
PY

curl -fsS --noproxy "*" --max-time 10 -X DELETE \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  "${DAEMON_URL}/api/v1/session?session_id=${FAIL_SESSION_ID_Q}" >/dev/null

echo "agentd_session_voice_webrtc_peer_runtime_smoke OK"
