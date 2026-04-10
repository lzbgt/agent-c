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
LOG_DIR="${ROOT}/build"
mkdir -p "${LOG_DIR}"
LOG_FILE="${LOG_DIR}/agentd_session_voice_webrtc_peer_builtin_smoke.log"

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

POSTGRES_NAME="agentd_voice_builtin_smoke"
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
export AGENTD_AUDIO_WEBRTC_BUILTIN_MODE="signaling_stub"
export AGENTD_AUDIO_WEBRTC_BROKER_URL="http://127.0.0.1:${BROKER_PORT}"
export AGENTD_AUDIO_WEBRTC_BROKER_TOKEN="audio-agentd-token"
agentd_smoke_start "${AGENTD_BIN}" "127.0.0.1" "${DAEMON_PORT}" "agentd_session_voice_webrtc_peer_builtin_smoke" >>"${LOG_FILE}" 2>&1
unset AGENTD_AUTH_TOKEN
unset AGENTD_AUDIO_WEBRTC_BUILTIN_MODE
unset AGENTD_AUDIO_WEBRTC_BROKER_URL
unset AGENTD_AUDIO_WEBRTC_BROKER_TOKEN

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

BROKER_SESSION_ID="$(python3 - "${START_JSON}" <<'PY'
import json, sys
obj = json.loads(sys.argv[1])
assert obj.get("ok") is True, obj
assert obj.get("builtin_available") is True, obj
peer = obj.get("peer") or {}
assert peer.get("runtime_kind") == "builtin", obj
assert peer.get("media_engine_kind") == "builtin_signaling_stub", obj
assert peer.get("media_engine_state") == "signaling_ready", obj
assert peer.get("media_events_total") == 2, obj
assert peer.get("media_answers_sent") == 0, obj
assert peer.get("media_remote_offers_seen") == 0, obj
assert peer.get("native_media_supported") is False, obj
assert peer.get("native_media_active") is False, obj
assert peer.get("running") is True and peer.get("ready") is True, obj
assert peer.get("managed_broker_session") is True, obj
assert obj.get("builtin_start_contract", {}).get("mutating_broker_actions_deferred") is False, obj
sid = peer.get("broker_session_id")
assert sid, obj
print(sid)
PY
)"

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

python3 - "${ANSWER_FILE}" <<'PY'
import json, sys
msg = json.load(open(sys.argv[1], "r", encoding="utf-8"))
payload = msg.get("payload") or {}
assert msg.get("type") == "answer", msg
assert payload.get("type") == "answer", msg
assert payload.get("sdp") == "stub-answer", msg
assert payload.get("sender_tag") == "agentd_runtime_peer", msg
PY

curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer audio-webui-token" \
  -H "Content-Type: application/json" \
  -d '{"type":"bye","payload":{"reason":"webui_done","sender_tag":"webui-peer"}}' \
  "http://127.0.0.1:${BROKER_PORT}/v1/audio/sessions/${BROKER_SESSION_ID}/signal" >/dev/null

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

echo "agentd_session_voice_webrtc_peer_builtin_smoke OK"
