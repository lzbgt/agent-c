#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "missing agentd binary path arg" >&2
  exit 2
fi

PORT_DAEMON="$(agentd_smoke_pick_port)"
PORT_FACADE="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"
NAME="agentd_codexw_local_api_facade_smoke"
LOG_DIR="$(agentd_smoke_log_dir)"
STATE_DIR="${LOG_DIR}/${NAME}_${PORT_DAEMON}.state"
FACADE_LOG="${LOG_DIR}/${NAME}_${PORT_FACADE}.facade.log"
FACADE_TOKEN="facade-smoke-token"
FACADE_URL="http://${HOST}:${PORT_FACADE}"

cleanup() {
  if [[ -n "${FACADE_PID:-}" ]]; then
    kill -TERM "${FACADE_PID}" >/dev/null 2>&1 || true
    wait "${FACADE_PID}" >/dev/null 2>&1 || true
  fi
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}" \
  --tools none \
  --state-dir "${STATE_DIR}"
agentd_smoke_wait_health "${DAEMON_URL}"

"${SCRIPT_DIR}/../tools/agentd_codexw_local_api_facade.py" \
  --host "${HOST}" \
  --port "${PORT_FACADE}" \
  --token "${FACADE_TOKEN}" \
  --agentd-base-url "${DAEMON_URL}" \
  --deployment-id "agentd-smoke" \
  --file-root "${SCRIPT_DIR}/.." \
  --turn-mode echo \
  --quiet \
  >"${FACADE_LOG}" 2>&1 &
FACADE_PID=$!

for _ in $(seq 1 100); do
  if curl -fsS --noproxy "*" -H "Authorization: Bearer ${FACADE_TOKEN}" "${FACADE_URL}/healthz" >/dev/null 2>&1; then
    break
  fi
  sleep 0.05
done

if ! curl -fsS --noproxy "*" -H "Authorization: Bearer ${FACADE_TOKEN}" "${FACADE_URL}/healthz" >/dev/null 2>&1; then
  echo "facade did not become healthy; log follows" >&2
  cat "${FACADE_LOG}" >&2 || true
  exit 1
fi

unauth_status="$(curl -sS --noproxy "*" -o /dev/null -w "%{http_code}" "${FACADE_URL}/api/v1/runtime")"
if [[ "${unauth_status}" != "401" ]]; then
  echo "expected unauthenticated runtime request to return 401, got ${unauth_status}" >&2
  exit 1
fi

runtime="$(curl -fsS --noproxy "*" -H "Authorization: Bearer ${FACADE_TOKEN}" "${FACADE_URL}/api/v1/runtime")"
runtime_status="$(curl -fsS --noproxy "*" -H "Authorization: Bearer ${FACADE_TOKEN}" "${FACADE_URL}/api/v1/runtime/status")"
runtime_sessions="$(curl -fsS --noproxy "*" -H "Authorization: Bearer ${FACADE_TOKEN}" "${FACADE_URL}/api/v1/runtime/sessions")"
runtime_events="$(curl -fsS --noproxy "*" -H "Authorization: Bearer ${FACADE_TOKEN}" "${FACADE_URL}/api/v1/runtime/events?limit=8")"
session="$(curl -fsS --noproxy "*" -H "Authorization: Bearer ${FACADE_TOKEN}" "${FACADE_URL}/api/v1/session")"
attach="$(curl -fsS --noproxy "*" \
  -H "Authorization: Bearer ${FACADE_TOKEN}" \
  -H "Content-Type: application/json" \
  -d '{"thread_id":"thread-smoke","client_id":"client_mobile","lease_seconds":120}' \
  "${FACADE_URL}/api/v1/session/attach")"
turn="$(curl -fsS --noproxy "*" \
  -H "Authorization: Bearer ${FACADE_TOKEN}" \
  -H "Content-Type: application/json" \
  -d '{"input":{"text":"hello broker facade"},"client_id":"client_mobile"}' \
  "${FACADE_URL}/api/v1/session/agentd/turn/start")"
transcript="$(curl -fsS --noproxy "*" \
  -H "Authorization: Bearer ${FACADE_TOKEN}" \
  "${FACADE_URL}/api/v1/session/agentd/transcript?limit=10")"
file_read="$(curl -fsS --noproxy "*" \
  -H "Authorization: Bearer ${FACADE_TOKEN}" \
  "${FACADE_URL}/api/v1/session/agentd/files/read?path=docs%2FWORKFLOWS.md&offset=0&limit=16")"
experience_action="$(curl -fsS --noproxy "*" \
  -H "Authorization: Bearer ${FACADE_TOKEN}" \
  -H "Content-Type: application/json" \
  -d '{"action":"experience.list","input":{"limit":5}}' \
  "${FACADE_URL}/api/v1/runtime/actions")"

python3 - <<PY
import base64, json, sys
runtime = json.loads(r'''${runtime}''')
runtime_status = json.loads(r'''${runtime_status}''')
runtime_sessions = json.loads(r'''${runtime_sessions}''')
runtime_events = json.loads(r'''${runtime_events}''')
session = json.loads(r'''${session}''')
attach = json.loads(r'''${attach}''')
turn = json.loads(r'''${turn}''')
transcript = json.loads(r'''${transcript}''')
file_read = json.loads(r'''${file_read}''')
experience_action = json.loads(r'''${experience_action}''')

if runtime.get("runtime", {}).get("kind") != "agentd":
    print("bad runtime", runtime, file=sys.stderr)
    raise SystemExit(1)
if runtime.get("runtime", {}).get("connection_mode") != "service":
    print("bad connection mode", runtime, file=sys.stderr)
    raise SystemExit(1)
runtime_caps = runtime.get("runtime", {}).get("runtime_capabilities", {})
if "workflow.submit" not in runtime_caps.get("actions", {}) or "experience.list" not in runtime_caps.get("actions", {}):
    print("missing runtime actions", runtime, file=sys.stderr)
    raise SystemExit(1)
for surface in ("sessions", "events", "status"):
    if runtime_caps.get("surfaces", {}).get(surface) is not True:
        print("missing runtime surface", surface, runtime_caps, file=sys.stderr)
        raise SystemExit(1)
for forbidden in ("runtime.restart", "runtime.update", "runtime.upgrade"):
    if forbidden in runtime_caps.get("actions", {}):
        print("agentd bridge must not advertise unsafe operator action", forbidden, runtime_caps, file=sys.stderr)
        raise SystemExit(1)
if runtime_sessions.get("runtime_kind") != "agentd" or not isinstance(runtime_sessions.get("sessions"), list):
    print("bad runtime sessions", runtime_sessions, file=sys.stderr)
    raise SystemExit(1)
if runtime_events.get("runtime_kind") != "agentd" or not isinstance(runtime_events.get("events"), list):
    print("bad runtime events", runtime_events, file=sys.stderr)
    raise SystemExit(1)
if runtime_status.get("runtime_kind") != "agentd" or runtime_status.get("update", {}).get("state") != "disabled":
    print("bad runtime status", runtime_status, file=sys.stderr)
    raise SystemExit(1)
if session.get("session_id") != "agentd" or session.get("session", {}).get("scope") != "process":
    print("bad session", session, file=sys.stderr)
    raise SystemExit(1)
if attach.get("thread_id") != "thread-smoke":
    print("bad attach", attach, file=sys.stderr)
    raise SystemExit(1)
if not turn.get("accepted") or turn.get("operation", {}).get("kind") != "turn.start":
    print("bad turn", turn, file=sys.stderr)
    raise SystemExit(1)
entries = transcript.get("transcript") or []
if transcript.get("total_entries") != 2 or len(entries) != 2:
    print("bad transcript count", transcript, file=sys.stderr)
    raise SystemExit(1)
if entries[0].get("role") != "user" or "hello broker facade" not in entries[0].get("text", ""):
    print("bad user transcript", transcript, file=sys.stderr)
    raise SystemExit(1)
if entries[1].get("role") != "assistant" or "agentd facade echo" not in entries[1].get("text", ""):
    print("bad assistant transcript", transcript, file=sys.stderr)
    raise SystemExit(1)
if file_read.get("path") != "docs/WORKFLOWS.md":
    print("bad file path", file_read, file=sys.stderr)
    raise SystemExit(1)
if not base64.b64decode(file_read.get("data_base64", "")).startswith(b"#"):
    print("bad file payload", file_read, file=sys.stderr)
    raise SystemExit(1)
if experience_action.get("action") != "experience.list" or not isinstance(experience_action.get("result"), dict):
    print("bad experience action", experience_action, file=sys.stderr)
    raise SystemExit(1)
PY

echo "agentd_codexw_local_api_facade_smoke OK"
