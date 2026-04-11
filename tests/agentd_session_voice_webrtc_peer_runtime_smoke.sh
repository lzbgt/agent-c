#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"
# shellcheck source=tests/lib/agentd_voice_webrtc_daemon_client.sh
source "${SCRIPT_DIR}/lib/agentd_voice_webrtc_daemon_client.sh"
# shellcheck source=tests/lib/agentd_voice_webrtc_broker_client.sh
source "${SCRIPT_DIR}/lib/agentd_voice_webrtc_broker_client.sh"

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
STATE_DIR="${LOG_DIR}/agentd_session_voice_webrtc_peer_runtime_smoke_${PORT_DAEMON}.state"

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

start_agentd_with_builtin_voice_default() {
  export AGENTD_AUTH_TOKEN="${DAEMON_TOKEN}"
  export AGENTD_AUDIO_WEBRTC_DEFAULT_RUNTIME_KIND="builtin"
  export AGENTD_AUDIO_WEBRTC_BROKER_URL="${VOICE_BROKER_URL}"
  export AGENTD_AUDIO_WEBRTC_BROKER_TOKEN="${VOICE_BROKER_TOKEN}"
  agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_session_voice_webrtc_peer_runtime_smoke_env_builtin" >>"${LOG_FILE}" 2>&1
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

restart_agentd_with_builtin_voice_default() {
  agentd_smoke_stop
  wait_daemon_stopped
  start_agentd_with_builtin_voice_default
  wait_daemon_ready
}

wait_daemon_ready

config_env_defaults="$(config_get)"

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

restart_agentd_with_builtin_voice_default

config_env_builtin_default="$(config_get)"

python3 - <<PY
import json, sys
obj = json.loads(r'''${config_env_builtin_default}''')
daemon = obj.get("daemon") or {}
audio = daemon.get("audio_webrtc") or {}
if audio.get("default_runtime_kind") != "builtin" or audio.get("default_runtime_kind_source") != "env":
  print("expected env builtin default_runtime_kind", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("default_runtime_kind_available") is not False:
  print("expected builtin env default to be unavailable", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("builtin_available") is not False or audio.get("bundled_available") is not True:
  print("unexpected builtin/bundled availability for env builtin default", obj, file=sys.stderr)
  raise SystemExit(1)
if "disabled" not in str(audio.get("builtin_unavailable_reason") or ""):
  print("expected builtin disabled unavailable reason for env builtin default", obj, file=sys.stderr)
  raise SystemExit(1)
if "disabled" not in str(audio.get("default_runtime_kind_unavailable_reason") or ""):
  print("expected builtin disabled default unavailable reason for env builtin default", obj, file=sys.stderr)
  raise SystemExit(1)
PY

ENV_BUILTIN_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_env_builtin_$(date +%s)_$RANDOM"
create_session "${ENV_BUILTIN_SESSION_ID}"

set +e
env_builtin_start_resp="$(curl -sS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${ENV_BUILTIN_SESSION_ID}",
  "action": "start",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-env-builtin-default",
  "sender_tag": "agentd_runtime_peer"
}))
PY
)" \
  -w $'\n%{http_code}' \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"
env_builtin_start_curl_rc=$?
set -e
if [[ ${env_builtin_start_curl_rc} -ne 0 ]]; then
  echo "voice_webrtc_peer env-builtin-default start request failed to complete" >&2
  exit 1
fi

env_builtin_start_code="$(printf '%s' "${env_builtin_start_resp}" | tail -n 1)"
env_builtin_start_body="$(printf '%s' "${env_builtin_start_resp}" | sed '$d')"
if [[ "${env_builtin_start_code}" != "501" ]]; then
  echo "expected env builtin default start to return http 501, got ${env_builtin_start_code}" >&2
  exit 1
fi

python3 - <<PY
import json, sys
obj = json.loads(r'''${env_builtin_start_body}''')
if obj.get("ok") is not False:
  print("expected env builtin default start to fail", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("default_runtime_kind") != "builtin" or obj.get("default_runtime_kind_source") != "env":
  print("expected builtin env default on start failure", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("default_runtime_kind_available") is not False:
  print("expected unavailable builtin env default on start failure", obj, file=sys.stderr)
  raise SystemExit(1)
if "disabled" not in str(obj.get("error") or ""):
  print("expected builtin disabled error for env default start", obj, file=sys.stderr)
  raise SystemExit(1)
PY

delete_session_quiet "${ENV_BUILTIN_SESSION_ID}"

restart_agentd

SESSION_DB_ID="agentd_session_voice_webrtc_peer_runtime_$(date +%s)_$RANDOM"
create_session "${SESSION_DB_ID}"

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
    status_body="$(voice_peer_status "${session_id}")"
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
    code="$(broker_session_status_code "${session_id}")"
    if [[ "${code}" == "404" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected deleted audio session after teardown, got status ${code}" >&2
  return 1
}

inject_stale_persisted_voice_runtime() {
  local session_id="$1"
  local runtime_kind="${2:-bundled}"
  python3 "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_record.py" write-stale \
    --db-path "${SESSION_DB_PATH}" \
    --state-dir "${STATE_DIR}" \
    --session-id "${session_id}" \
    --runtime-kind "${runtime_kind}" \
    --broker-url "${VOICE_BROKER_URL}" \
    --peer-tool "${PEER_TOOL}"
}

run_receiver_peer() {
  local session_id="$1"
  ROOT_PATH="${ROOT}" node "${SCRIPT_DIR}/lib/agentd_voice_webrtc_peer_receiver.js" \
    "http://127.0.0.1:${BROKER_PORT}" \
    "${session_id}" \
    "audio-webui-token" \
    "webui_playwright_peer" >>"${LOG_FILE}" 2>&1
}

start_resp="$(voice_peer_request "$(python3 - <<PY
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
)")"

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

already_running_resp="$(voice_peer_request "{\"session_id\":\"${SESSION_DB_ID}\",\"action\":\"start\"}")"

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

persisted_conflict_body="${LOG_DIR}/voice_webrtc_peer_persisted_conflict_body.json"
persisted_conflict_status="$(curl -sS --noproxy "*" --max-time 10 -o "${persisted_conflict_body}" -w '%{http_code}' \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${SESSION_DB_ID}\",\"action\":\"start\",\"runtime_kind\":\"builtin\"}" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"
if [[ "${persisted_conflict_status}" != "409" ]]; then
  echo "expected persisted running builtin conflict to return 409, got ${persisted_conflict_status}" >&2
  cat "${persisted_conflict_body}" >&2
  exit 1
fi
python3 - <<PY
import json, sys
obj = json.load(open(r'''${persisted_conflict_body}''', 'r', encoding='utf-8'))
peer = obj.get("peer") or {}
if obj.get("error") != "voice peer already running with different config":
  print("unexpected persisted running conflict response", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("status_source") != "persisted" or peer.get("runtime_kind") != "bundled":
  print("unexpected persisted conflict peer snapshot", obj, file=sys.stderr)
  raise SystemExit(1)
PY

run_receiver_peer "${BROKER_SESSION_ID}"

SESSION_JSON="$(broker_session_get "${BROKER_SESSION_ID}")"

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

broker_session_signal "${BROKER_SESSION_ID}" '{"type":"bye","payload":{"reason":"webui_done","sender_tag":"webui_playwright_peer"}}'

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
  -d "{\"session_id\":\"${SESSION_DB_ID}\",\"action\":\"start\",\"runtime_kind\":\"builtin\",\"broker_agent_id\":\"a-1\",\"broker_deployment_id\":\"lab-builtin-contract\"}" \
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
if "disabled" not in str(obj.get("builtin_unavailable_reason", "")):
  print("expected builtin disabled unavailable reason", obj, file=sys.stderr)
  raise SystemExit(1)
if "disabled" not in str(obj.get("error", "")):
  print("expected builtin disabled error", obj, file=sys.stderr)
  raise SystemExit(1)
contract = obj.get("builtin_start_contract") or {}
if contract.get("runtime_kind") != "builtin" or contract.get("signaling_surface") != "voice_webrtc_peer":
  print("expected builtin start contract metadata", obj, file=sys.stderr)
  raise SystemExit(1)
if contract.get("mutating_broker_actions_deferred") is not True:
  print("expected builtin contract to defer mutating broker actions", obj, file=sys.stderr)
  raise SystemExit(1)
sequence = contract.get("startup_sequence") or []
if [step.get("stage") for step in sequence] != [
    "auto_create_broker_session",
    "launch_runtime",
    "startup_confirmation",
    "startup_failure_cleanup",
]:
  print("expected builtin startup sequence", obj, file=sys.stderr)
  raise SystemExit(1)
if not sequence[0].get("deferred", False) or not sequence[1].get("deferred", False):
  print("expected builtin startup sequence to remain deferred", obj, file=sys.stderr)
  raise SystemExit(1)
broker_session = contract.get("broker_session") or {}
if broker_session.get("mode") != "auto_create" or broker_session.get("agent_id") != "a-1":
  print("expected builtin auto-create broker contract", obj, file=sys.stderr)
  raise SystemExit(1)
if broker_session.get("deployment_id") != "lab-builtin-contract":
  print("expected builtin deployment contract", obj, file=sys.stderr)
  raise SystemExit(1)
if broker_session.get("session_id") is not None:
  print("expected builtin auto-create broker contract to omit session_id", obj, file=sys.stderr)
  raise SystemExit(1)
artifacts = contract.get("runtime_artifacts") or {}
runtime_dir = artifacts.get("runtime_dir") or ""
if not runtime_dir.endswith("/voice_webrtc_peers/" + obj.get("session_id", "")):
  print("expected builtin runtime_dir contract", obj, file=sys.stderr)
  raise SystemExit(1)
if artifacts.get("ready_file_path") != runtime_dir + "/ready.json":
  print("expected builtin ready_file_path contract", obj, file=sys.stderr)
  raise SystemExit(1)
if artifacts.get("stdout_log_path") != runtime_dir + "/stdout.jsonl":
  print("expected builtin stdout_log_path contract", obj, file=sys.stderr)
  raise SystemExit(1)
if artifacts.get("stderr_log_path") != runtime_dir + "/stderr.log":
  print("expected builtin stderr_log_path contract", obj, file=sys.stderr)
  raise SystemExit(1)
if artifacts.get("stdout_format") != "jsonl" or artifacts.get("stderr_format") != "text":
  print("expected builtin runtime artifact format contract", obj, file=sys.stderr)
  raise SystemExit(1)
media_plan = contract.get("media_runtime_plan") or {}
if media_plan.get("schema") != "voice_webrtc_peer_media_runtime_plan_v1":
  print("expected builtin media runtime plan schema", obj, file=sys.stderr)
  raise SystemExit(1)
if media_plan.get("signaling_surface") != "voice_webrtc_peer":
  print("expected builtin media runtime signaling surface", obj, file=sys.stderr)
  raise SystemExit(1)
if media_plan.get("runtime_kind") != "builtin" or media_plan.get("session_id") != obj.get("session_id"):
  print("expected builtin media runtime plan identity", obj, file=sys.stderr)
  raise SystemExit(1)
if media_plan.get("broker_session_id") is not None:
  print("expected builtin auto-create media runtime plan to omit broker_session_id", obj, file=sys.stderr)
  raise SystemExit(1)
if media_plan.get("managed_broker_session") is not True:
  print("expected builtin media runtime plan managed broker session", obj, file=sys.stderr)
  raise SystemExit(1)
if media_plan.get("broker_agent_id") != "a-1" or media_plan.get("broker_deployment_id") != "lab-builtin-contract":
  print("expected builtin media runtime plan broker ownership metadata", obj, file=sys.stderr)
  raise SystemExit(1)
if media_plan.get("ready_signal") != "ready_file" or media_plan.get("ready_file_path") != artifacts.get("ready_file_path"):
  print("expected builtin media runtime plan ready contract", obj, file=sys.stderr)
  raise SystemExit(1)
if media_plan.get("deadline_ms") != contract.get("deadline_ms") or media_plan.get("poll_interval_ms") != contract.get("poll_interval_ms") or media_plan.get("tone_hz") != contract.get("tone_hz"):
  print("expected builtin media runtime timing contract", obj, file=sys.stderr)
  raise SystemExit(1)
planned_runtime = contract.get("planned_runtime") or {}
if planned_runtime.get("schema") != "session_voice_webrtc_peer_runtime_v1":
  print("expected builtin planned runtime schema", obj, file=sys.stderr)
  raise SystemExit(1)
if planned_runtime.get("status_source") != "planned":
  print("expected builtin planned runtime status_source=planned", obj, file=sys.stderr)
  raise SystemExit(1)
if planned_runtime.get("runtime_kind") != "builtin" or planned_runtime.get("session_id") != obj.get("session_id"):
  print("expected builtin planned runtime identity", obj, file=sys.stderr)
  raise SystemExit(1)
if planned_runtime.get("tool_path") != "@builtin" or planned_runtime.get("node_bin") != "@builtin":
  print("expected builtin planned runtime builtin execution sentinels", obj, file=sys.stderr)
  raise SystemExit(1)
if planned_runtime.get("managed_broker_session") is not True:
  print("expected builtin planned runtime managed broker session", obj, file=sys.stderr)
  raise SystemExit(1)
if planned_runtime.get("running") is not False or planned_runtime.get("ready") is not False:
  print("expected builtin planned runtime to stay inactive", obj, file=sys.stderr)
  raise SystemExit(1)
if planned_runtime.get("stdout_log_path") != artifacts.get("stdout_log_path"):
  print("expected builtin planned runtime stdout_log_path to match artifacts", obj, file=sys.stderr)
  raise SystemExit(1)
if planned_runtime.get("broker_agent_id") != "a-1" or planned_runtime.get("broker_deployment_id") != "lab-builtin-contract":
  print("expected builtin planned runtime broker ownership metadata", obj, file=sys.stderr)
  raise SystemExit(1)
peer = obj.get("peer") or {}
if peer.get("schema") != "session_voice_webrtc_peer_runtime_v1":
  print("expected builtin top-level peer preview schema", obj, file=sys.stderr)
  raise SystemExit(1)
for key in ("runtime_kind", "session_id", "broker_url", "stdout_log_path", "stderr_log_path", "ready_file_path"):
  if peer.get(key) != planned_runtime.get(key):
    print(f"expected builtin top-level peer preview to match planned_runtime for {key}", obj, file=sys.stderr)
    raise SystemExit(1)
if peer.get("status_source") != "planned":
  print("expected builtin top-level peer preview status_source=planned", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("tool_path") != "@builtin" or peer.get("node_bin") != "@builtin":
  print("expected builtin top-level peer preview builtin execution sentinels", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("managed_broker_session") is not True or peer.get("running") is not False or peer.get("ready") is not False:
  print("expected builtin top-level peer preview state", obj, file=sys.stderr)
  raise SystemExit(1)
if "disabled" not in str(peer.get("last_error") or ""):
  print("expected builtin top-level peer preview disabled error", obj, file=sys.stderr)
  raise SystemExit(1)
PY

BUILTIN_BORROWED_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_builtin_borrowed_$(date +%s)_$RANDOM"
create_session "${BUILTIN_BORROWED_SESSION_ID}"

builtin_borrowed_broker_create_resp="$(broker_session_create '{"agent_id":"a-1","mode":"webrtc"}')"

BUILTIN_BORROWED_BROKER_SESSION_ID="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${builtin_borrowed_broker_create_resp}''')
sid = str(obj.get("session_id") or "").strip()
if not obj.get("ok") or not sid:
  print("failed to create builtin borrowed broker session", obj, file=sys.stderr)
  raise SystemExit(1)
print(sid)
PY
)"

builtin_borrowed_resp_body="${LOG_DIR}/voice_webrtc_peer_builtin_borrowed_body.json"
builtin_borrowed_status="$(curl -sS --noproxy "*" --max-time 10 -o "${builtin_borrowed_resp_body}" -w '%{http_code}' \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${BUILTIN_BORROWED_SESSION_ID}",
  "action": "start",
  "runtime_kind": "builtin",
  "broker_session_id": "${BUILTIN_BORROWED_BROKER_SESSION_ID}",
  "broker_url": "${VOICE_BROKER_URL}",
  "broker_token": "${VOICE_BROKER_TOKEN}",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 551
}))
PY
)" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"
if [[ "${builtin_borrowed_status}" != "501" ]]; then
  echo "expected builtin borrowed start to return 501, got ${builtin_borrowed_status}" >&2
  cat "${builtin_borrowed_resp_body}" >&2
  exit 1
fi
python3 - <<PY
import json, sys
obj = json.load(open(r'''${builtin_borrowed_resp_body}''', 'r', encoding='utf-8'))
if obj.get("ok") is not False:
  print("expected builtin borrowed start to fail not-implemented", obj, file=sys.stderr)
  raise SystemExit(1)
contract = obj.get("builtin_start_contract") or {}
broker_session = contract.get("broker_session") or {}
if broker_session.get("mode") != "borrowed":
  print("expected builtin borrowed broker contract", obj, file=sys.stderr)
  raise SystemExit(1)
if broker_session.get("session_id") != r'''${BUILTIN_BORROWED_BROKER_SESSION_ID}''' or broker_session.get("preflighted") is not True:
  print("expected builtin borrowed broker session id/preflight", obj, file=sys.stderr)
  raise SystemExit(1)
if broker_session.get("session_mode") != "webrtc":
  print("expected builtin borrowed broker session mode", obj, file=sys.stderr)
  raise SystemExit(1)
if broker_session.get("agent_id") is not None or broker_session.get("deployment_id") is not None:
  print("expected builtin borrowed broker contract to omit auto-create ownership fields", obj, file=sys.stderr)
  raise SystemExit(1)
media_plan = contract.get("media_runtime_plan") or {}
if media_plan.get("broker_session_id") != r'''${BUILTIN_BORROWED_BROKER_SESSION_ID}''' or media_plan.get("managed_broker_session") is not False:
  print("expected builtin borrowed media runtime plan", obj, file=sys.stderr)
  raise SystemExit(1)
if media_plan.get("broker_agent_id") is not None or media_plan.get("broker_deployment_id") is not None:
  print("expected builtin borrowed media runtime plan to omit ownership fields", obj, file=sys.stderr)
  raise SystemExit(1)
planned_runtime = contract.get("planned_runtime") or {}
peer = obj.get("peer") or {}
if planned_runtime.get("broker_session_id") != r'''${BUILTIN_BORROWED_BROKER_SESSION_ID}''' or planned_runtime.get("managed_broker_session") is not False:
  print("expected builtin borrowed planned runtime", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("broker_session_id") != r'''${BUILTIN_BORROWED_BROKER_SESSION_ID}''' or peer.get("managed_broker_session") is not False:
  print("expected builtin borrowed top-level peer preview", obj, file=sys.stderr)
  raise SystemExit(1)
PY

builtin_borrowed_delete_status="$(broker_session_delete_status "${BUILTIN_BORROWED_BROKER_SESSION_ID}")"
if [[ "${builtin_borrowed_delete_status}" != "200" && "${builtin_borrowed_delete_status}" != "404" ]]; then
  echo "expected builtin borrowed broker session delete to return 200 or 404, got ${builtin_borrowed_delete_status}" >&2
  exit 1
fi

BUILTIN_MISSING_BROKER_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_builtin_missing_broker_$(date +%s)_$RANDOM"
create_session "${BUILTIN_MISSING_BROKER_SESSION_ID}"

builtin_missing_broker_session_resp_body="${LOG_DIR}/voice_webrtc_peer_builtin_missing_broker_session_body.json"
builtin_missing_broker_session_status="$(curl -sS --noproxy "*" --max-time 10 -o "${builtin_missing_broker_session_resp_body}" -w '%{http_code}' \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${BUILTIN_MISSING_BROKER_SESSION_ID}",
  "action": "start",
  "runtime_kind": "builtin",
  "broker_session_id": "missing-builtin-broker-session",
  "sender_tag": "agentd_runtime_peer"
}))
PY
)" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"
if [[ "${builtin_missing_broker_session_status}" != "400" ]]; then
  echo "expected builtin missing broker_session_id start to return 400, got ${builtin_missing_broker_session_status}" >&2
  cat "${builtin_missing_broker_session_resp_body}" >&2
  exit 1
fi
python3 - <<PY
import json, sys
obj = json.load(open(r'''${builtin_missing_broker_session_resp_body}''', 'r', encoding='utf-8'))
if obj.get("ok") is not False:
  print("expected builtin missing broker_session_id start to fail", obj, file=sys.stderr)
  raise SystemExit(1)
if "broker_session_id not found" not in str(obj.get("error", "")):
  print("expected builtin missing broker_session_id error", obj, file=sys.stderr)
  raise SystemExit(1)
PY

builtin_missing_broker_session_status_json="$(voice_peer_status "${BUILTIN_MISSING_BROKER_SESSION_ID}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${builtin_missing_broker_session_status_json}''')
if obj.get("running") is not False or obj.get("peer") is not None:
  print("expected no runtime after builtin missing broker_session_id preflight failure", obj, file=sys.stderr)
  raise SystemExit(1)
PY

BUILTIN_CONFLICT_BROKER_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_builtin_conflict_broker_$(date +%s)_$RANDOM"
create_session "${BUILTIN_CONFLICT_BROKER_SESSION_ID}"

builtin_conflict_broker_session_resp_body="${LOG_DIR}/voice_webrtc_peer_builtin_conflict_broker_session_body.json"
builtin_conflict_broker_session_status="$(curl -sS --noproxy "*" --max-time 10 -o "${builtin_conflict_broker_session_resp_body}" -w '%{http_code}' \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${BUILTIN_CONFLICT_BROKER_SESSION_ID}",
  "action": "start",
  "runtime_kind": "builtin",
  "broker_session_id": "aud_conflict_builtin",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-builtin-conflict",
  "sender_tag": "agentd_runtime_peer"
}))
PY
)" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"
if [[ "${builtin_conflict_broker_session_status}" != "400" ]]; then
  echo "expected conflicting builtin broker session start to return 400, got ${builtin_conflict_broker_session_status}" >&2
  cat "${builtin_conflict_broker_session_resp_body}" >&2
  exit 1
fi
python3 - <<PY
import json, sys
obj = json.load(open(r'''${builtin_conflict_broker_session_resp_body}''', 'r', encoding='utf-8'))
if obj.get("ok") is not False:
  print("expected conflicting builtin broker session start to fail", obj, file=sys.stderr)
  raise SystemExit(1)
if "must be omitted" not in str(obj.get("error", "")):
  print("expected conflicting builtin broker session validation error", obj, file=sys.stderr)
  raise SystemExit(1)
PY

builtin_conflict_broker_session_status_json="$(voice_peer_status "${BUILTIN_CONFLICT_BROKER_SESSION_ID}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${builtin_conflict_broker_session_status_json}''')
if obj.get("running") is not False or obj.get("peer") is not None:
  print("expected no runtime after conflicting builtin broker session validation failure", obj, file=sys.stderr)
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
create_session "${MISSING_BROKER_SESSION_ID}"

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

missing_broker_session_status_json="$(voice_peer_status "${MISSING_BROKER_SESSION_ID}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${missing_broker_session_status_json}''')
if obj.get("running") is not False or obj.get("peer") is not None:
  print("expected no runtime after missing broker_session_id preflight failure", obj, file=sys.stderr)
  raise SystemExit(1)
PY

delete_session_quiet "${MISSING_BROKER_SESSION_ID}"

CONFLICT_BROKER_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_conflict_broker_$(date +%s)_$RANDOM"
create_session "${CONFLICT_BROKER_SESSION_ID}"

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

conflict_broker_session_status_json="$(voice_peer_status "${CONFLICT_BROKER_SESSION_ID}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${conflict_broker_session_status_json}''')
if obj.get("running") is not False or obj.get("peer") is not None:
  print("expected no runtime after conflicting broker session validation failure", obj, file=sys.stderr)
  raise SystemExit(1)
PY

delete_session_quiet "${CONFLICT_BROKER_SESSION_ID}"

start_resp2="$(voice_peer_request "$(python3 - <<PY
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
)")"

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

session_exists_status="$(broker_session_status_code "${BROKER_SESSION_ID2}")"
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

stop_resp="$(voice_peer_request "{\"session_id\":\"${SESSION_DB_ID}\",\"action\":\"stop\"}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${stop_resp}''')
if not obj.get("ok"):
  print("voice_webrtc_peer stop failed", obj, file=sys.stderr)
  raise SystemExit(1)
peer = obj.get("peer") or {}
if peer.get("runtime_kind") != "bundled":
  print("unexpected stopped peer runtime_kind", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("stopped") is not False or obj.get("reason") != "not_running":
  print("expected stop to report not_running for already-exited peer", obj, file=sys.stderr)
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

RECOVERED_STOP_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_recovered_stop_$(date +%s)_$RANDOM"
create_session "${RECOVERED_STOP_SESSION_ID}"

recovered_stop_start_resp="$(voice_peer_request "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${RECOVERED_STOP_SESSION_ID}",
  "action": "start",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-recovered-stop",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 587
}))
PY
)")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${recovered_stop_start_resp}''')
if not obj.get("ok") or not obj.get("started"):
  print("recovered-stop voice start failed", obj, file=sys.stderr)
  raise SystemExit(1)
peer = obj.get("peer") or {}
if peer.get("runtime_kind") != "bundled" or peer.get("managed_broker_session") is not True:
  print("recovered-stop voice start missing bundled managed runtime", obj, file=sys.stderr)
  raise SystemExit(1)
PY

RECOVERED_STOP_BROKER_SESSION_ID="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${recovered_stop_start_resp}''')
peer = obj.get("peer") or {}
sid = str(peer.get("broker_session_id") or "").strip()
if not sid:
  print("missing recovered-stop broker_session_id in start response", file=sys.stderr)
  raise SystemExit(1)
print(sid)
PY
)"

wait_voice_peer_ready "${RECOVERED_STOP_SESSION_ID}" 1 status_json

restart_agentd
wait_voice_peer_ready "${RECOVERED_STOP_SESSION_ID}" 1 status_json

python3 - <<PY
import json, sys
obj = json.loads(r'''${status_json}''')
peer = obj.get("peer") or {}
if peer.get("status_source") != "persisted" or peer.get("runtime_kind") != "bundled":
  print("expected persisted bundled peer after recovered-stop restart", obj, file=sys.stderr)
  raise SystemExit(1)
if not peer.get("running") or not peer.get("ready"):
  print("expected recovered-stop peer to stay running after restart", obj, file=sys.stderr)
  raise SystemExit(1)
PY

recovered_stop_resp="$(voice_peer_request "{\"session_id\":\"${RECOVERED_STOP_SESSION_ID}\",\"action\":\"stop\"}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${recovered_stop_resp}''')
if not obj.get("ok") or obj.get("stopped") is not True:
  print("recovered-stop voice stop failed", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("broker_session_deleted") is not True:
  print("expected broker_session_deleted after recovered-stop voice stop", obj, file=sys.stderr)
  raise SystemExit(1)
peer = obj.get("peer") or {}
if peer.get("status_source") != "persisted" or peer.get("runtime_kind") != "bundled":
  print("recovered-stop voice stop lost persisted bundled snapshot", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("running"):
  print("recovered-stop voice stop still reported running peer", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("exit_signal") != 15:
  print("recovered-stop voice stop missing synthesized SIGTERM result", obj, file=sys.stderr)
  raise SystemExit(1)
PY

wait_voice_peer_ready "${RECOVERED_STOP_SESSION_ID}" 0 stopped_json

python3 - <<PY
import json, sys
obj = json.loads(r'''${stopped_json}''')
peer = obj.get("peer") or {}
if peer.get("status_source") != "persisted" or peer.get("running"):
  print("recovered-stop final status wrong", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("exit_signal") != 15:
  print("recovered-stop final status did not persist SIGTERM result", obj, file=sys.stderr)
  raise SystemExit(1)
PY

wait_broker_session_deleted "${RECOVERED_STOP_BROKER_SESSION_ID}"

RECOVERED_DELETE_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_recovered_delete_$(date +%s)_$RANDOM"
create_session "${RECOVERED_DELETE_SESSION_ID}"

recovered_delete_start_resp="$(voice_peer_request "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${RECOVERED_DELETE_SESSION_ID}",
  "action": "start",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-recovered-delete",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 611
}))
PY
)")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${recovered_delete_start_resp}''')
if not obj.get("ok") or not obj.get("started"):
  print("recovered-delete voice start failed", obj, file=sys.stderr)
  raise SystemExit(1)
peer = obj.get("peer") or {}
if peer.get("runtime_kind") != "bundled" or peer.get("managed_broker_session") is not True:
  print("recovered-delete voice start missing bundled managed runtime", obj, file=sys.stderr)
  raise SystemExit(1)
PY

RECOVERED_DELETE_BROKER_SESSION_ID="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${recovered_delete_start_resp}''')
peer = obj.get("peer") or {}
sid = str(peer.get("broker_session_id") or "").strip()
if not sid:
  print("missing recovered-delete broker_session_id in start response", file=sys.stderr)
  raise SystemExit(1)
print(sid)
PY
)"

RECOVERED_DELETE_STDOUT_LOG="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${recovered_delete_start_resp}''')
peer = obj.get("peer") or {}
path = str(peer.get("stdout_log_path") or "").strip()
if not path:
  print("missing recovered-delete stdout_log_path in start response", file=sys.stderr)
  raise SystemExit(1)
print(path)
PY
)"

wait_voice_peer_ready "${RECOVERED_DELETE_SESSION_ID}" 1 status_json

restart_agentd
wait_voice_peer_ready "${RECOVERED_DELETE_SESSION_ID}" 1 status_json

python3 - <<PY
import json, sys
obj = json.loads(r'''${status_json}''')
peer = obj.get("peer") or {}
if peer.get("status_source") != "persisted" or peer.get("runtime_kind") != "bundled":
  print("expected persisted bundled peer before recovered-delete session erase", obj, file=sys.stderr)
  raise SystemExit(1)
if not peer.get("running") or not peer.get("ready"):
  print("expected recovered-delete peer to stay running after restart", obj, file=sys.stderr)
  raise SystemExit(1)
PY

recovered_delete_resp="$(delete_session "${RECOVERED_DELETE_SESSION_ID}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${recovered_delete_resp}''')
if not obj.get("ok") or obj.get("deleted_from_db") is not True:
  print("recovered-delete session delete failed", obj, file=sys.stderr)
  raise SystemExit(1)
cleanup = obj.get("voice_runtime_cleanup") or {}
if cleanup.get("runtime_present") is not True or cleanup.get("runtime_was_running") is not True:
  print("recovered-delete cleanup missing running runtime", obj, file=sys.stderr)
  raise SystemExit(1)
if cleanup.get("stopped") is not True:
  print("recovered-delete cleanup should report stopped=true", obj, file=sys.stderr)
  raise SystemExit(1)
if cleanup.get("broker_session_delete_attempted") is not True or cleanup.get("broker_session_deleted") is not True:
  print("recovered-delete cleanup missing broker session deletion", obj, file=sys.stderr)
  raise SystemExit(1)
if cleanup.get("persisted_record_cleared") is not True or cleanup.get("runtime_artifacts_deleted") is not True:
  print("recovered-delete cleanup missing persisted/artifact cleanup", obj, file=sys.stderr)
  raise SystemExit(1)
peer = cleanup.get("peer") or {}
if peer.get("status_source") != "persisted" or peer.get("runtime_kind") != "bundled":
  print("recovered-delete cleanup lost persisted bundled peer snapshot", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("running"):
  print("recovered-delete cleanup still reported running peer", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("exit_signal") != 15:
  print("recovered-delete cleanup missing synthesized SIGTERM result", obj, file=sys.stderr)
  raise SystemExit(1)
PY

recovered_delete_status="$(voice_peer_status "${RECOVERED_DELETE_SESSION_ID}")"

python3 - <<PY
import json, os, sys
obj = json.loads(r'''${recovered_delete_status}''')
if obj.get("session_exists") is not False or obj.get("running") is not False or obj.get("peer") is not None:
  print("expected no peer state after recovered-delete cleanup", obj, file=sys.stderr)
  raise SystemExit(1)
if os.path.exists(r'''${RECOVERED_DELETE_STDOUT_LOG}'''):
  print("expected recovered-delete stdout log to be removed", r'''${RECOVERED_DELETE_STDOUT_LOG}''', file=sys.stderr)
  raise SystemExit(1)
PY

restart_agentd

recovered_delete_status_restart="$(voice_peer_status "${RECOVERED_DELETE_SESSION_ID}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${recovered_delete_status_restart}''')
if obj.get("session_exists") is not False or obj.get("running") is not False or obj.get("peer") is not None:
  print("expected no resurrected peer state after recovered-delete restart", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("cleanup_on_missing_session") not in (None, {}):
  print("unexpected cleanup_on_missing_session after recovered-delete cleanup", obj, file=sys.stderr)
  raise SystemExit(1)
PY

wait_broker_session_deleted "${RECOVERED_DELETE_BROKER_SESSION_ID}"

start_resp3="$(voice_peer_request "$(python3 - <<PY
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
)")"

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

SESSION_DB_ID_Q="$(url_quote "${SESSION_DB_ID}")"
delete_resp="$(delete_session "${SESSION_DB_ID}")"

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

status_after_delete="$(voice_peer_status "${SESSION_DB_ID}")"

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

status_after_delete_restart="$(voice_peer_status "${SESSION_DB_ID}")"

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
create_session "${STALE_SESSION_ID}"

start_resp4="$(voice_peer_request "$(python3 - <<PY
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
)")"

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

python3 "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_record.py" delete-session \
  --db-path "${SESSION_DB_PATH}" \
  --session-id "${STALE_SESSION_ID}"

status_after_stale_delete="$(voice_peer_status "${STALE_SESSION_ID}")"

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

config_update_resp="$(config_update "$(python3 - <<PY
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
)")"

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

config_persisted_defaults="$(config_get)"

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
create_session "${CONFIG_SESSION_ID}"

config_start_resp="$(voice_peer_request "$(python3 - <<PY
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
)")"

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

config_stop_resp="$(voice_peer_request "{\"session_id\":\"${CONFIG_SESSION_ID}\",\"action\":\"stop\"}")"

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

delete_session_quiet "${CONFIG_SESSION_ID}"

EXTERNAL_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_external_$(date +%s)_$RANDOM"
create_session "${EXTERNAL_SESSION_ID}"

external_config_start_resp="$(voice_peer_request "$(python3 - <<PY
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
)")"

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

external_config_start_again_resp="$(voice_peer_request "$(python3 - <<PY
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
)")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${external_config_start_again_resp}''')
peer = obj.get("peer") or {}
if not obj.get("ok") or obj.get("already_running") is not True:
  print("expected idempotent already_running response for external runtime", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("runtime_kind") != "external" or peer.get("tone_hz") != 901:
  print("unexpected peer snapshot for idempotent external start", obj, file=sys.stderr)
  raise SystemExit(1)
PY

external_default_conflict_update_resp="$(config_update "$(python3 - <<PY
import json
print(json.dumps({
  "audio_webrtc": {
    "broker_url": "${VOICE_BROKER_URL}",
    "broker_token": "${VOICE_BROKER_TOKEN}",
    "peer_tool_path": "${PEER_TOOL}",
    "default_runtime_kind": "bundled",
    "node_bin": "node"
  }
}))
PY
)")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${external_default_conflict_update_resp}''')
audio = obj.get("audio_webrtc") or {}
if not obj.get("ok"):
  print("failed to switch audio_webrtc default runtime_kind for conflict proof", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("default_runtime_kind") != "bundled" or audio.get("default_runtime_kind_source") != "config":
  print("expected bundled config-backed default for running-conflict proof", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("bundled_available") is not True or audio.get("external_available") is not True:
  print("expected both bundled and external available for running-conflict proof", obj, file=sys.stderr)
  raise SystemExit(1)
PY

external_default_conflict_status_resp="$(voice_peer_status "${EXTERNAL_SESSION_ID}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${external_default_conflict_status_resp}''')
peer = obj.get("peer") or {}
drift = obj.get("backend_policy_drift") or {}
fields = set(drift.get("changed_fields") or [])
current = drift.get("current_effective_start") or {}
if peer.get("runtime_kind") != "external" or obj.get("running") is not True:
  print("expected running external peer while proving default-runtime drift", obj, file=sys.stderr)
  raise SystemExit(1)
if "default_runtime_kind" not in fields:
  print("expected default_runtime_kind drift on status", obj, file=sys.stderr)
  raise SystemExit(1)
if current.get("runtime_kind") != "bundled" or current.get("default_runtime_kind_source") != "config":
  print("unexpected effective start policy for default-runtime drift", obj, file=sys.stderr)
  raise SystemExit(1)
if current.get("runtime_available") is not True:
  print("expected bundled effective start policy to remain available", obj, file=sys.stderr)
  raise SystemExit(1)
PY

external_default_conflict_body="${LOG_DIR}/voice_webrtc_peer_external_default_conflict_body.json"
external_default_conflict_status="$(curl -sS --noproxy "*" --max-time 10 -o "${external_default_conflict_body}" -w '%{http_code}' \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${EXTERNAL_SESSION_ID}\",\"action\":\"start\"}" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"
if [[ "${external_default_conflict_status}" != "409" ]]; then
  echo "expected effective default-runtime conflict to return 409, got ${external_default_conflict_status}" >&2
  cat "${external_default_conflict_body}" >&2
  exit 1
fi
python3 - <<PY
import json, sys
obj = json.load(open(r'''${external_default_conflict_body}''', 'r', encoding='utf-8'))
peer = obj.get("peer") or {}
drift = obj.get("backend_policy_drift") or {}
fields = set(drift.get("changed_fields") or [])
current = drift.get("current_effective_start") or {}
if obj.get("error") != "voice peer already running with different config":
  print("unexpected effective default-runtime conflict response", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("runtime_kind") != "external" or peer.get("tone_hz") != 901:
  print("unexpected peer snapshot for effective default-runtime conflict", obj, file=sys.stderr)
  raise SystemExit(1)
if "default_runtime_kind" not in fields:
  print("expected default_runtime_kind drift on conflict response", obj, file=sys.stderr)
  raise SystemExit(1)
if current.get("runtime_kind") != "bundled" or current.get("default_runtime_kind_source") != "config":
  print("unexpected conflict effective start policy for default-runtime drift", obj, file=sys.stderr)
  raise SystemExit(1)
PY

external_unavailable_conflict_update_resp="$(config_update "$(python3 - <<PY
import json
print(json.dumps({
  "audio_webrtc": {
    "broker_url": "${VOICE_BROKER_URL}",
    "broker_token": "${VOICE_BROKER_TOKEN}",
    "peer_tool_path": None,
    "default_runtime_kind": "external",
    "node_bin": "node"
  }
}))
PY
)")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${external_unavailable_conflict_update_resp}''')
audio = obj.get("audio_webrtc") or {}
if not obj.get("ok"):
  print("failed to clear audio_webrtc peer_tool_path for unavailable-conflict proof", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("peer_tool_path_configured") is not False:
  print("expected cleared peer_tool_path_configured for unavailable-conflict proof", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("default_runtime_kind") != "external" or audio.get("default_runtime_kind_source") != "config":
  print("expected external config-backed default for unavailable-conflict proof", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("external_available") is not False or audio.get("default_runtime_kind_available") is not False:
  print("expected unavailable external backend for unavailable-conflict proof", obj, file=sys.stderr)
  raise SystemExit(1)
PY

external_unavailable_conflict_body="${LOG_DIR}/voice_webrtc_peer_external_unavailable_conflict_body.json"
external_unavailable_conflict_status="$(curl -sS --noproxy "*" --max-time 10 -o "${external_unavailable_conflict_body}" -w '%{http_code}' \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "{\"session_id\":\"${EXTERNAL_SESSION_ID}\",\"action\":\"start\"}" \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"
if [[ "${external_unavailable_conflict_status}" != "409" ]]; then
  echo "expected unavailable-backend running conflict to return 409, got ${external_unavailable_conflict_status}" >&2
  cat "${external_unavailable_conflict_body}" >&2
  exit 1
fi
python3 - <<PY
import json, sys
obj = json.load(open(r'''${external_unavailable_conflict_body}''', 'r', encoding='utf-8'))
peer = obj.get("peer") or {}
drift = obj.get("backend_policy_drift") or {}
fields = set(drift.get("changed_fields") or [])
current = drift.get("current_effective_start") or {}
if obj.get("error") != "voice peer already running with different config":
  print("unexpected unavailable-backend running conflict response", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("runtime_kind") != "external" or peer.get("tool_path") != r'''${PEER_TOOL}''':
  print("unexpected peer snapshot for unavailable-backend running conflict", obj, file=sys.stderr)
  raise SystemExit(1)
if "peer_tool_path" not in fields:
  print("expected peer_tool_path drift on unavailable external conflict", obj, file=sys.stderr)
  raise SystemExit(1)
if current.get("runtime_kind") != "external" or current.get("runtime_available") is not False:
  print("unexpected effective start policy for unavailable external conflict", obj, file=sys.stderr)
  raise SystemExit(1)
if "not configured" not in str(current.get("runtime_unavailable_reason") or ""):
  print("expected missing peer_tool_path reason on unavailable external conflict", obj, file=sys.stderr)
  raise SystemExit(1)
PY

external_node_bin_conflict_update_resp="$(config_update "$(python3 - <<PY
import json
print(json.dumps({
  "audio_webrtc": {
    "broker_url": "${VOICE_BROKER_URL}",
    "broker_token": "${VOICE_BROKER_TOKEN}",
    "peer_tool_path": "${PEER_TOOL}",
    "default_runtime_kind": "external",
    "node_bin": "definitely-not-a-real-node-binary"
  }
}))
PY
)")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${external_node_bin_conflict_update_resp}''')
audio = obj.get("audio_webrtc") or {}
if not obj.get("ok"):
  print("failed to change audio_webrtc node_bin for conflict proof", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("default_runtime_kind") != "external" or audio.get("default_runtime_kind_source") != "config":
  print("expected restored external config-backed default before node_bin conflict", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("node_bin") != "definitely-not-a-real-node-binary":
  print("expected persisted invalid node_bin for conflict proof", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("external_available") is not False or audio.get("default_runtime_kind_available") is not False:
  print("expected unavailable external backend for invalid node_bin conflict proof", obj, file=sys.stderr)
  raise SystemExit(1)
PY

external_node_bin_conflict_body="${LOG_DIR}/voice_webrtc_peer_external_node_bin_conflict_body.json"
external_node_bin_conflict_status="$(voice_peer_request_status "${external_node_bin_conflict_body}" "$(python3 - <<PY
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
)")"
if [[ "${external_node_bin_conflict_status}" != "409" ]]; then
  echo "expected effective node_bin conflict to return 409, got ${external_node_bin_conflict_status}" >&2
  cat "${external_node_bin_conflict_body}" >&2
  exit 1
fi
python3 - <<PY
import json, sys
obj = json.load(open(r'''${external_node_bin_conflict_body}''', 'r', encoding='utf-8'))
peer = obj.get("peer") or {}
drift = obj.get("backend_policy_drift") or {}
fields = set(drift.get("changed_fields") or [])
current = drift.get("current_effective_start") or {}
if obj.get("error") != "voice peer already running with different config":
  print("unexpected effective node_bin conflict response", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("runtime_kind") != "external" or peer.get("node_bin") != "node":
  print("unexpected peer snapshot for effective node_bin conflict", obj, file=sys.stderr)
  raise SystemExit(1)
if "node_bin" not in fields:
  print("expected node_bin drift on node_bin conflict", obj, file=sys.stderr)
  raise SystemExit(1)
if current.get("runtime_kind") != "external" or current.get("node_bin") != "definitely-not-a-real-node-binary":
  print("unexpected effective start policy for node_bin conflict", obj, file=sys.stderr)
  raise SystemExit(1)
if current.get("runtime_available") is not False or "not found" not in str(current.get("runtime_unavailable_reason") or ""):
  print("expected unavailable node_bin drift reason on conflict", obj, file=sys.stderr)
  raise SystemExit(1)
PY

external_restore_valid_config_resp="$(config_update "$(python3 - <<PY
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
)")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${external_restore_valid_config_resp}''')
audio = obj.get("audio_webrtc") or {}
if not obj.get("ok"):
  print("failed to restore valid external config after running-conflict proofs", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("external_available") is not True or audio.get("default_runtime_kind_available") is not True:
  print("expected restored external availability after running-conflict proofs", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("node_bin") != "node" or audio.get("peer_tool_path_configured") is not True:
  print("expected restored node_bin/tool_path after running-conflict proofs", obj, file=sys.stderr)
  raise SystemExit(1)
PY

external_restored_status_resp="$(voice_peer_status "${EXTERNAL_SESSION_ID}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${external_restored_status_resp}''')
if obj.get("running") is not True or (obj.get("peer") or {}).get("runtime_kind") != "external":
  print("expected running external peer after restoring valid config", obj, file=sys.stderr)
  raise SystemExit(1)
if "backend_policy_drift" in obj:
  print("did not expect backend_policy_drift after restoring valid external config", obj, file=sys.stderr)
  raise SystemExit(1)
PY

external_conflict_body="${LOG_DIR}/voice_webrtc_peer_external_conflict_body.json"
external_conflict_status="$(voice_peer_request_status "${external_conflict_body}" "{\"session_id\":\"${EXTERNAL_SESSION_ID}\",\"action\":\"start\",\"runtime_kind\":\"external\",\"tone_hz\":1234}")"
if [[ "${external_conflict_status}" != "409" ]]; then
  echo "expected external running conflict to return 409, got ${external_conflict_status}" >&2
  cat "${external_conflict_body}" >&2
  exit 1
fi
python3 - <<PY
import json, sys
obj = json.load(open(r'''${external_conflict_body}''', 'r', encoding='utf-8'))
peer = obj.get("peer") or {}
if obj.get("error") != "voice peer already running with different config":
  print("unexpected external running conflict response", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("runtime_kind") != "external" or peer.get("tone_hz") != 901:
  print("unexpected external conflict peer snapshot", obj, file=sys.stderr)
  raise SystemExit(1)
if "backend_policy_drift" in obj:
  print("did not expect backend_policy_drift for explicit request conflict with restored policy", obj, file=sys.stderr)
  raise SystemExit(1)
PY

external_config_stop_resp="$(voice_peer_request "{\"session_id\":\"${EXTERNAL_SESSION_ID}\",\"action\":\"stop\",\"runtime_kind\":\"builtin\"}")"

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

invalid_broker_defaults_resp="$(config_update "$(python3 - <<PY
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
)")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${invalid_broker_defaults_resp}''')
if not obj.get("ok"):
  print("failed to set invalid broker token defaults for lazy-validation proof", obj, file=sys.stderr)
  raise SystemExit(1)
PY

MANAGED_BAD_STOP_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_managed_bad_stop_$(date +%s)_$RANDOM"
create_session "${MANAGED_BAD_STOP_SESSION_ID}"

managed_bad_stop_start_resp="$(voice_peer_request "$(python3 - <<PY
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
)")"

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

managed_bad_stop_resp="$(voice_peer_request "{\"session_id\":\"${MANAGED_BAD_STOP_SESSION_ID}\",\"action\":\"stop\"}")"

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

managed_bad_stop_broker_status="$(broker_session_status_code "${MANAGED_BAD_STOP_BROKER_SESSION_ID}")"
if [[ "${managed_bad_stop_broker_status}" != "200" && "${managed_bad_stop_broker_status}" != "404" ]]; then
  echo "expected managed bad-stop broker session inspect to return 200 or 404, got ${managed_bad_stop_broker_status}" >&2
  exit 1
fi

managed_bad_stop_delete_status="$(broker_session_delete_status "${MANAGED_BAD_STOP_BROKER_SESSION_ID}")"
if [[ "${managed_bad_stop_delete_status}" != "200" && "${managed_bad_stop_delete_status}" != "404" ]]; then
  echo "expected managed bad-stop broker session delete to return 200 or 404, got ${managed_bad_stop_delete_status}" >&2
  exit 1
fi

MANAGED_BAD_DELETE_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_managed_bad_delete_$(date +%s)_$RANDOM"
create_session "${MANAGED_BAD_DELETE_SESSION_ID}"

managed_bad_delete_start_resp="$(voice_peer_request "$(python3 - <<PY
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
)")"

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

managed_bad_delete_resp="$(delete_session "${MANAGED_BAD_DELETE_SESSION_ID}")"

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

managed_bad_delete_broker_status="$(broker_session_status_code "${MANAGED_BAD_DELETE_BROKER_SESSION_ID}")"
if [[ "${managed_bad_delete_broker_status}" != "200" && "${managed_bad_delete_broker_status}" != "404" ]]; then
  echo "expected managed bad-delete broker session inspect to return 200 or 404, got ${managed_bad_delete_broker_status}" >&2
  exit 1
fi

managed_bad_delete_delete_status="$(broker_session_delete_status "${MANAGED_BAD_DELETE_BROKER_SESSION_ID}")"
if [[ "${managed_bad_delete_delete_status}" != "200" && "${managed_bad_delete_delete_status}" != "404" ]]; then
  echo "expected managed bad-delete broker session delete to return 200 or 404, got ${managed_bad_delete_delete_status}" >&2
  exit 1
fi

BORROWED_STOP_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_borrowed_stop_$(date +%s)_$RANDOM"
create_session "${BORROWED_STOP_SESSION_ID}"

borrowed_stop_broker_create_resp="$(broker_session_create '{"agent_id":"a-1","mode":"webrtc"}')"

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

borrowed_stop_start_resp="$(voice_peer_request "$(python3 - <<PY
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
)")"

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

borrowed_stop_resp="$(voice_peer_request "{\"session_id\":\"${BORROWED_STOP_SESSION_ID}\",\"action\":\"stop\"}")"

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

borrowed_stop_delete_status="$(broker_session_delete_status "${BORROWED_STOP_BROKER_SESSION_ID}")"
if [[ "${borrowed_stop_delete_status}" != "200" && "${borrowed_stop_delete_status}" != "404" ]]; then
  echo "expected borrowed stop broker session delete to return 200 or 404, got ${borrowed_stop_delete_status}" >&2
  exit 1
fi

BORROWED_DELETE_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_borrowed_delete_$(date +%s)_$RANDOM"
create_session "${BORROWED_DELETE_SESSION_ID}"

borrowed_delete_broker_create_resp="$(broker_session_create '{"agent_id":"a-1","mode":"webrtc"}')"

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

borrowed_delete_start_resp="$(voice_peer_request "$(python3 - <<PY
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
)")"

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

borrowed_delete_resp="$(delete_session "${BORROWED_DELETE_SESSION_ID}")"

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

borrowed_delete_broker_delete_status="$(broker_session_delete_status "${BORROWED_DELETE_BROKER_SESSION_ID}")"
if [[ "${borrowed_delete_broker_delete_status}" != "200" && "${borrowed_delete_broker_delete_status}" != "404" ]]; then
  echo "expected borrowed delete broker session delete to return 200 or 404, got ${borrowed_delete_broker_delete_status}" >&2
  exit 1
fi

restore_broker_defaults_resp="$(config_update "$(python3 - <<PY
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
)")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${restore_broker_defaults_resp}''')
if not obj.get("ok"):
  print("failed to restore broker defaults after lazy-validation proof", obj, file=sys.stderr)
  raise SystemExit(1)
PY

NOOP_STOP_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_noop_stop_$(date +%s)_$RANDOM"
create_session "${NOOP_STOP_SESSION_ID}"

noop_stop_resp="$(voice_peer_request "{\"session_id\":\"${NOOP_STOP_SESSION_ID}\",\"action\":\"stop\",\"runtime_kind\":\"not-a-real-runtime-kind\"}")"

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

delete_session_quiet "${NOOP_STOP_SESSION_ID}"

delete_session_quiet "${EXTERNAL_SESSION_ID}"

unavailable_default_config_resp="$(config_update "$(python3 - <<PY
import json
print(json.dumps({
  "audio_webrtc": {
    "peer_tool_path": None,
    "default_runtime_kind": "external",
    "node_bin": "node"
  }
}))
PY
)")"

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

unavailable_default_config_get="$(config_get)"

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
create_session "${UNAVAILABLE_DEFAULT_SESSION_ID}"

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

delete_session_quiet "${UNAVAILABLE_DEFAULT_SESSION_ID}"

builtin_default_config_resp="$(config_update "$(python3 - <<PY
import json
print(json.dumps({
  "audio_webrtc": {
    "peer_tool_path": "${PEER_TOOL}",
    "default_runtime_kind": "builtin",
    "node_bin": "node"
  }
}))
PY
)")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${builtin_default_config_resp}''')
audio = obj.get("audio_webrtc") or {}
if not obj.get("ok"):
  print("config update for builtin default failed", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("default_runtime_kind") != "builtin" or audio.get("default_runtime_kind_source") != "config":
  print("expected config-backed builtin default", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("default_runtime_kind_available") is not False:
  print("expected config-backed builtin default to be unavailable", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("builtin_available") is not False or audio.get("bundled_available") is not True or audio.get("external_available") is not True:
  print("unexpected backend availability for config-backed builtin default", obj, file=sys.stderr)
  raise SystemExit(1)
if "disabled" not in str(audio.get("builtin_unavailable_reason") or ""):
  print("expected builtin disabled unavailable reason for config-backed builtin default", obj, file=sys.stderr)
  raise SystemExit(1)
if "disabled" not in str(audio.get("default_runtime_kind_unavailable_reason") or ""):
  print("expected builtin disabled default unavailable reason for config-backed builtin default", obj, file=sys.stderr)
  raise SystemExit(1)
PY

restart_agentd_without_voice_defaults

builtin_default_config_get="$(config_get)"

python3 - <<PY
import json, sys
obj = json.loads(r'''${builtin_default_config_get}''')
daemon = obj.get("daemon") or {}
audio = daemon.get("audio_webrtc") or {}
if audio.get("default_runtime_kind") != "builtin" or audio.get("default_runtime_kind_source") != "config":
  print("expected persisted builtin default after restart", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("default_runtime_kind_available") is not False:
  print("expected persisted builtin default to remain unavailable after restart", obj, file=sys.stderr)
  raise SystemExit(1)
if "disabled" not in str(audio.get("default_runtime_kind_unavailable_reason") or ""):
  print("expected builtin disabled default unavailable reason after restart", obj, file=sys.stderr)
  raise SystemExit(1)
PY

BUILTIN_DEFAULT_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_default_builtin_$(date +%s)_$RANDOM"
create_session "${BUILTIN_DEFAULT_SESSION_ID}"

set +e
builtin_default_start_resp="$(curl -sS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${DAEMON_TOKEN}" \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${BUILTIN_DEFAULT_SESSION_ID}",
  "action": "start",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-default-builtin",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 904
}))
PY
)" \
  -w $'\n%{http_code}' \
  "${DAEMON_URL}/api/v1/session/voice_webrtc_peer")"
builtin_default_start_curl_rc=$?
set -e
if [[ ${builtin_default_start_curl_rc} -ne 0 ]]; then
  echo "voice_webrtc_peer config-builtin-default start request failed to complete" >&2
  exit 1
fi

builtin_default_start_code="$(printf '%s' "${builtin_default_start_resp}" | tail -n 1)"
builtin_default_start_body="$(printf '%s' "${builtin_default_start_resp}" | sed '$d')"
if [[ "${builtin_default_start_code}" != "501" ]]; then
  echo "expected config builtin default start to return http 501, got ${builtin_default_start_code}" >&2
  exit 1
fi

python3 - <<PY
import json, sys
obj = json.loads(r'''${builtin_default_start_body}''')
if obj.get("ok") is not False:
  print("expected config builtin default start to fail", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("default_runtime_kind") != "builtin" or obj.get("default_runtime_kind_source") != "config":
  print("expected config-backed builtin default on start failure", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("default_runtime_kind_available") is not False:
  print("expected unavailable builtin config default on start failure", obj, file=sys.stderr)
  raise SystemExit(1)
if "disabled" not in str(obj.get("error") or ""):
  print("expected builtin disabled error for config default start", obj, file=sys.stderr)
  raise SystemExit(1)
PY

delete_session_quiet "${BUILTIN_DEFAULT_SESSION_ID}"

restored_external_default_config_resp="$(config_update "$(python3 - <<PY
import json
print(json.dumps({
  "audio_webrtc": {
    "peer_tool_path": "${PEER_TOOL}",
    "default_runtime_kind": "external",
    "node_bin": "node"
  }
}))
PY
)")"

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

invalid_default_self_healed="$(config_get)"

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
create_session "${SELF_HEAL_SESSION_ID}"

self_heal_start_resp="$(voice_peer_request "$(python3 - <<PY
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
)")"

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

self_heal_stop_resp="$(voice_peer_request "{\"session_id\":\"${SELF_HEAL_SESSION_ID}\",\"action\":\"stop\"}")"

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

delete_session_quiet "${SELF_HEAL_SESSION_ID}"

CORRUPT_RUNTIME_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_corrupt_record_$(date +%s)_$RANDOM"
create_session "${CORRUPT_RUNTIME_SESSION_ID}"

CORRUPT_RUNTIME_DIR="${STATE_DIR}/voice_webrtc_peers/${CORRUPT_RUNTIME_SESSION_ID}"
python3 "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_record.py" write-corrupt \
  --db-path "${SESSION_DB_PATH}" \
  --session-id "${CORRUPT_RUNTIME_SESSION_ID}" \
  --value "{ definitely-not-json" \
  --runtime-dir "${CORRUPT_RUNTIME_DIR}" \
  --artifact-json '{"stale":"artifact"}'

corrupt_runtime_status="$(voice_peer_status "${CORRUPT_RUNTIME_SESSION_ID}")"

printf '%s' "${corrupt_runtime_status}" | python3 "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_record.py" assert-cleared \
  --db-path "${SESSION_DB_PATH}" \
  --session-id "${CORRUPT_RUNTIME_SESSION_ID}" \
  --runtime-dir "${CORRUPT_RUNTIME_DIR}" \
  --cleanup-key cleanup_on_corrupt_record \
  --label "corrupt runtime status" \
  --expect-session-exists true \
  --expect-running false

python3 "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_record.py" write-corrupt \
  --db-path "${SESSION_DB_PATH}" \
  --session-id "${CORRUPT_RUNTIME_SESSION_ID}" \
  --value "{ definitely-not-json-again"

corrupt_runtime_start_resp="$(voice_peer_request "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${CORRUPT_RUNTIME_SESSION_ID}",
  "action": "start",
  "runtime_kind": "bundled",
  "broker_url": "${VOICE_BROKER_URL}",
  "broker_token": "${VOICE_BROKER_TOKEN}",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-corrupt-runtime-self-heal",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 903
}))
PY
)")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${corrupt_runtime_start_resp}''')
peer = obj.get("peer") or {}
cleanup = obj.get("cleanup_on_corrupt_record") or {}
if not obj.get("ok") or not obj.get("started"):
  print("expected start to recover after corrupt runtime self-heal", obj, file=sys.stderr)
  raise SystemExit(1)
if cleanup.get("persisted_record_cleared") is not True:
  print("expected start response to report corrupt runtime self-heal", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("runtime_kind") != "bundled":
  print("expected bundled runtime after corrupt runtime self-heal", obj, file=sys.stderr)
  raise SystemExit(1)
PY

CORRUPT_RUNTIME_BROKER_SESSION_ID="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${corrupt_runtime_start_resp}''')
peer = obj.get("peer") or {}
sid = str(peer.get("broker_session_id") or "").strip()
if not sid:
  print("missing corrupt runtime self-heal broker_session_id", file=sys.stderr)
  raise SystemExit(1)
print(sid)
PY
)"

wait_voice_peer_ready "${CORRUPT_RUNTIME_SESSION_ID}" 1 status_json bundled bundled auto 1

PLANNED_RUNTIME_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_planned_record_$(date +%s)_$RANDOM"
create_session "${PLANNED_RUNTIME_SESSION_ID}"

PLANNED_RUNTIME_DIR="${STATE_DIR}/voice_webrtc_peers/${PLANNED_RUNTIME_SESSION_ID}"
python3 "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_record.py" write-planned \
  --db-path "${SESSION_DB_PATH}" \
  --runtime-dir "${PLANNED_RUNTIME_DIR}" \
  --session-id "${PLANNED_RUNTIME_SESSION_ID}" \
  --broker-url "${VOICE_BROKER_URL}"

planned_runtime_status="$(voice_peer_status "${PLANNED_RUNTIME_SESSION_ID}")"

printf '%s' "${planned_runtime_status}" | python3 "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_record.py" assert-cleared \
  --db-path "${SESSION_DB_PATH}" \
  --session-id "${PLANNED_RUNTIME_SESSION_ID}" \
  --runtime-dir "${PLANNED_RUNTIME_DIR}" \
  --cleanup-key cleanup_on_corrupt_record \
  --label "planned runtime status" \
  --expect-session-exists true \
  --expect-running false

corrupt_runtime_stop_resp="$(voice_peer_request "{\"session_id\":\"${CORRUPT_RUNTIME_SESSION_ID}\",\"action\":\"stop\"}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${corrupt_runtime_stop_resp}''')
if not obj.get("ok") or not obj.get("stopped"):
  print("expected corrupt runtime self-heal stop to succeed", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("broker_session_deleted") is not True:
  print("expected corrupt runtime self-heal stop to delete broker session", obj, file=sys.stderr)
  raise SystemExit(1)
PY

wait_broker_session_deleted "${CORRUPT_RUNTIME_BROKER_SESSION_ID}"

delete_session_quiet "${CORRUPT_RUNTIME_SESSION_ID}"

STALE_STATUS_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_stale_status_$(date +%s)_$RANDOM"
create_session "${STALE_STATUS_SESSION_ID}"
inject_stale_persisted_voice_runtime "${STALE_STATUS_SESSION_ID}"
STALE_STATUS_RUNTIME_DIR="${STATE_DIR}/voice_webrtc_peers/${STALE_STATUS_SESSION_ID}"
stale_status_resp="$(voice_peer_status "${STALE_STATUS_SESSION_ID}")"

printf '%s' "${stale_status_resp}" | python3 "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_record.py" assert-cleared \
  --db-path "${SESSION_DB_PATH}" \
  --session-id "${STALE_STATUS_SESSION_ID}" \
  --runtime-dir "${STALE_STATUS_RUNTIME_DIR}" \
  --cleanup-key cleanup_on_stale_record \
  --label "stale runtime status" \
  --expect-running false

delete_session_quiet "${STALE_STATUS_SESSION_ID}"

STALE_STOP_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_stale_stop_$(date +%s)_$RANDOM"
create_session "${STALE_STOP_SESSION_ID}"
inject_stale_persisted_voice_runtime "${STALE_STOP_SESSION_ID}"
STALE_STOP_RUNTIME_DIR="${STATE_DIR}/voice_webrtc_peers/${STALE_STOP_SESSION_ID}"
stale_stop_resp="$(voice_peer_request "{\"session_id\":\"${STALE_STOP_SESSION_ID}\",\"action\":\"stop\"}")"

printf '%s' "${stale_stop_resp}" | python3 "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_record.py" assert-cleared \
  --db-path "${SESSION_DB_PATH}" \
  --session-id "${STALE_STOP_SESSION_ID}" \
  --runtime-dir "${STALE_STOP_RUNTIME_DIR}" \
  --cleanup-key cleanup_on_stale_record \
  --label "stale runtime stop" \
  --expect-stopped false \
  --expect-reason not_running

delete_session_quiet "${STALE_STOP_SESSION_ID}"

STALE_START_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_stale_start_$(date +%s)_$RANDOM"
create_session "${STALE_START_SESSION_ID}"
inject_stale_persisted_voice_runtime "${STALE_START_SESSION_ID}"
STALE_START_RUNTIME_DIR="${STATE_DIR}/voice_webrtc_peers/${STALE_START_SESSION_ID}"

stale_start_resp="$(voice_peer_request "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${STALE_START_SESSION_ID}",
  "action": "start",
  "runtime_kind": "bundled",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-stale-runtime-self-heal",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 911
}))
PY
)")"

python3 - <<PY
import json, os, sys
obj = json.loads(r'''${stale_start_resp}''')
peer = obj.get("peer") or {}
cleanup = obj.get("cleanup_on_stale_record") or {}
if not obj.get("ok") or not obj.get("started"):
  print("expected start to recover after stale runtime self-heal", obj, file=sys.stderr)
  raise SystemExit(1)
if cleanup.get("persisted_record_cleared") is not True:
  print("expected start response to report stale runtime self-heal", obj, file=sys.stderr)
  raise SystemExit(1)
if cleanup.get("runtime_artifacts_deleted") is not True:
  print("expected start response to delete stale runtime artifacts", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("runtime_kind") != "bundled" or not peer.get("running"):
  print("expected bundled running runtime after stale runtime self-heal", obj, file=sys.stderr)
  raise SystemExit(1)
if os.path.exists(r'''${STALE_START_RUNTIME_DIR}/stdout.jsonl'''):
  with open(r'''${STALE_START_RUNTIME_DIR}/stdout.jsonl''', 'r', encoding='utf-8') as f:
    if '{"stale":"artifact"}' in f.read():
      print("expected stale stdout artifact to be replaced on fresh start", file=sys.stderr)
      raise SystemExit(1)
PY

STALE_START_BROKER_SESSION_ID="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${stale_start_resp}''')
peer = obj.get("peer") or {}
sid = str(peer.get("broker_session_id") or "").strip()
if not sid:
  print("missing stale runtime self-heal broker_session_id", file=sys.stderr)
  raise SystemExit(1)
print(sid)
PY
)"

wait_voice_peer_ready "${STALE_START_SESSION_ID}" 1 status_json bundled bundled auto 1

stale_start_stop_resp="$(voice_peer_request "{\"session_id\":\"${STALE_START_SESSION_ID}\",\"action\":\"stop\"}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${stale_start_stop_resp}''')
if not obj.get("ok") or not obj.get("stopped"):
  print("expected stale runtime self-heal stop to succeed", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("broker_session_deleted") is not True:
  print("expected stale runtime self-heal stop to delete broker session", obj, file=sys.stderr)
  raise SystemExit(1)
PY

wait_broker_session_deleted "${STALE_START_BROKER_SESSION_ID}"

delete_session_quiet "${STALE_START_SESSION_ID}"

BUILTIN_STALE_STATUS_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_builtin_stale_status_$(date +%s)_$RANDOM"
create_session "${BUILTIN_STALE_STATUS_SESSION_ID}"
inject_stale_persisted_voice_runtime "${BUILTIN_STALE_STATUS_SESSION_ID}" builtin
BUILTIN_STALE_STATUS_RUNTIME_DIR="${STATE_DIR}/voice_webrtc_peers/${BUILTIN_STALE_STATUS_SESSION_ID}"
builtin_stale_status_resp="$(voice_peer_status "${BUILTIN_STALE_STATUS_SESSION_ID}")"

printf '%s' "${builtin_stale_status_resp}" | python3 "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_record.py" assert-cleared \
  --db-path "${SESSION_DB_PATH}" \
  --session-id "${BUILTIN_STALE_STATUS_SESSION_ID}" \
  --runtime-dir "${BUILTIN_STALE_STATUS_RUNTIME_DIR}" \
  --cleanup-key cleanup_on_stale_record \
  --label "builtin stale runtime status" \
  --expect-running false

delete_session_quiet "${BUILTIN_STALE_STATUS_SESSION_ID}"

BUILTIN_STALE_STOP_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_builtin_stale_stop_$(date +%s)_$RANDOM"
create_session "${BUILTIN_STALE_STOP_SESSION_ID}"
inject_stale_persisted_voice_runtime "${BUILTIN_STALE_STOP_SESSION_ID}" builtin
BUILTIN_STALE_STOP_RUNTIME_DIR="${STATE_DIR}/voice_webrtc_peers/${BUILTIN_STALE_STOP_SESSION_ID}"
builtin_stale_stop_resp="$(voice_peer_request "{\"session_id\":\"${BUILTIN_STALE_STOP_SESSION_ID}\",\"action\":\"stop\"}")"

printf '%s' "${builtin_stale_stop_resp}" | python3 "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_record.py" assert-cleared \
  --db-path "${SESSION_DB_PATH}" \
  --session-id "${BUILTIN_STALE_STOP_SESSION_ID}" \
  --runtime-dir "${BUILTIN_STALE_STOP_RUNTIME_DIR}" \
  --cleanup-key cleanup_on_stale_record \
  --label "builtin stale runtime stop" \
  --expect-stopped false \
  --expect-reason not_running

delete_session_quiet "${BUILTIN_STALE_STOP_SESSION_ID}"

BUILTIN_STALE_START_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_builtin_stale_start_$(date +%s)_$RANDOM"
create_session "${BUILTIN_STALE_START_SESSION_ID}"
inject_stale_persisted_voice_runtime "${BUILTIN_STALE_START_SESSION_ID}" builtin
BUILTIN_STALE_START_RUNTIME_DIR="${STATE_DIR}/voice_webrtc_peers/${BUILTIN_STALE_START_SESSION_ID}"

builtin_stale_start_resp="$(voice_peer_request "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${BUILTIN_STALE_START_SESSION_ID}",
  "action": "start",
  "runtime_kind": "bundled",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-builtin-stale-runtime-self-heal",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 913
}))
PY
)")"

python3 - <<PY
import json, os, sys
obj = json.loads(r'''${builtin_stale_start_resp}''')
peer = obj.get("peer") or {}
cleanup = obj.get("cleanup_on_stale_record") or {}
if not obj.get("ok") or not obj.get("started"):
  print("expected start to recover after builtin stale runtime self-heal", obj, file=sys.stderr)
  raise SystemExit(1)
if cleanup.get("persisted_record_cleared") is not True:
  print("expected start response to report builtin stale runtime self-heal", obj, file=sys.stderr)
  raise SystemExit(1)
if cleanup.get("runtime_artifacts_deleted") is not True:
  print("expected start response to delete builtin stale runtime artifacts", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("runtime_kind") != "bundled" or not peer.get("running"):
  print("expected bundled running runtime after builtin stale runtime self-heal", obj, file=sys.stderr)
  raise SystemExit(1)
if os.path.exists(r'''${BUILTIN_STALE_START_RUNTIME_DIR}/stdout.jsonl'''):
  with open(r'''${BUILTIN_STALE_START_RUNTIME_DIR}/stdout.jsonl''', 'r', encoding='utf-8') as f:
    if '{"stale":"artifact"}' in f.read():
      print("expected builtin stale stdout artifact to be replaced on fresh start", file=sys.stderr)
      raise SystemExit(1)
PY

BUILTIN_STALE_START_BROKER_SESSION_ID="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${builtin_stale_start_resp}''')
peer = obj.get("peer") or {}
sid = str(peer.get("broker_session_id") or "").strip()
if not sid:
  print("missing builtin stale runtime self-heal broker_session_id", file=sys.stderr)
  raise SystemExit(1)
print(sid)
PY
)"

wait_voice_peer_ready "${BUILTIN_STALE_START_SESSION_ID}" 1 status_json bundled bundled auto 1

builtin_stale_start_stop_resp="$(voice_peer_request "{\"session_id\":\"${BUILTIN_STALE_START_SESSION_ID}\",\"action\":\"stop\"}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${builtin_stale_start_stop_resp}''')
if not obj.get("ok") or not obj.get("stopped"):
  print("expected builtin stale runtime self-heal stop to succeed", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("broker_session_deleted") is not True:
  print("expected builtin stale runtime self-heal stop to delete broker session", obj, file=sys.stderr)
  raise SystemExit(1)
PY

wait_broker_session_deleted "${BUILTIN_STALE_START_BROKER_SESSION_ID}"

delete_session_quiet "${BUILTIN_STALE_START_SESSION_ID}"

config_invalid_node_bin_update_resp="$(config_update "$(python3 - <<PY
import json
print(json.dumps({
  "audio_webrtc": {
    "node_bin": "definitely-not-a-real-node-binary"
  }
}))
PY
)")"

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

invalid_node_bin_config_json="$(config_get)"

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
create_session "${INVALID_NODE_BIN_SESSION_ID}"

invalid_node_bin_start_body="${LOG_DIR}/voice_webrtc_peer_invalid_node_bin_start_body.json"
invalid_node_bin_start_status="$(voice_peer_request_status "${invalid_node_bin_start_body}" "$(python3 - <<PY
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
)")"
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

invalid_node_bin_status_json="$(voice_peer_status "${INVALID_NODE_BIN_SESSION_ID}")"

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

delete_session_quiet "${INVALID_NODE_BIN_SESSION_ID}"

config_failfast_update_resp="$(config_update "$(python3 - <<PY
import json
print(json.dumps({
  "audio_webrtc": {
    "node_bin": "false"
  }
}))
PY
)")"

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
create_session "${FAIL_SESSION_ID}"

fail_start_body="${LOG_DIR}/voice_webrtc_peer_fail_start_body.json"
fail_start_status="$(voice_peer_request_status "${fail_start_body}" "$(python3 - <<PY
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
)")"
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

fail_status_json="$(voice_peer_status "${FAIL_SESSION_ID}")"

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

delete_session_quiet "${FAIL_SESSION_ID}"

echo "agentd_session_voice_webrtc_peer_runtime_smoke OK"
