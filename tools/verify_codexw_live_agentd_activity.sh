#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CODEXW_ROOT="${CODEXW_ROOT:-$(cd "${ROOT}/.." && pwd)/codexw}"
BROKER_URL="${CODEXW_BROKER_BASE_URL:-https://broker.hubstack.cn}"
BROKER_ADMIN="${CODEXW_BROKER_ADMIN:-${CODEXW_ROOT}/scripts/broker-admin}"
AGENTD_BIN="${AGENTD_BIN:-${ROOT}/build/agentd}"
KEEP_DEPLOYMENT="${KEEP_DEPLOYMENT:-0}"
NAME="codexw_live_agentd_activity"

if [[ ! -x "${AGENTD_BIN}" ]]; then
  echo "agentd binary not found or not executable: ${AGENTD_BIN}" >&2
  exit 2
fi
if [[ ! -x "${BROKER_ADMIN}" ]]; then
  echo "codexw broker-admin not found or not executable: ${BROKER_ADMIN}" >&2
  exit 2
fi

pick_port() {
  python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

json_field() {
  python3 -c 'import json,sys; obj=json.load(sys.stdin); cur=obj
for part in sys.argv[1].split("."):
    cur = cur[part]
print(cur)' "$1"
}

RUN_ID="$(date +%Y%m%d%H%M%S)"
DEPLOYMENT_ID="${DEPLOYMENT_ID:-agentd-live-proof-${RUN_ID}}"
TOKEN_ID="${TOKEN_ID:-agentd-live-proof-${RUN_ID}}"
RUN_DIR="${RUN_DIR:-${ROOT}/build/${NAME}-${DEPLOYMENT_ID}}"
IDENTITY_DIR="${RUN_DIR}/native-identity"
STATE_DIR="${RUN_DIR}/state"
DB_PATH="${RUN_DIR}/agentd.sqlite"
AGENTD_PORT="$(pick_port)"
AGENTD_TOKEN="agentd-live-proof-${RUN_ID}"
AGENTD_URL="http://127.0.0.1:${AGENTD_PORT}"
LAUNCHER_LOG="${RUN_DIR}/launcher.log"
PROOF_JSON="${RUN_DIR}/proof.json"

mkdir -p "${RUN_DIR}"

LAUNCHER_PID=""
cleanup_launcher() {
  if [[ -n "${LAUNCHER_PID}" ]]; then
    kill -TERM "${LAUNCHER_PID}" >/dev/null 2>&1 || true
    wait "${LAUNCHER_PID}" >/dev/null 2>&1 || true
    LAUNCHER_PID=""
  fi
}

cleanup() {
  cleanup_launcher
  if [[ "${KEEP_DEPLOYMENT}" != "1" ]]; then
    "${BROKER_ADMIN}" --timeout-seconds 120 deployment-delete --deployment-id "${DEPLOYMENT_ID}" >/dev/null 2>&1 || true
    "${BROKER_ADMIN}" --timeout-seconds 120 enrollment-token-delete --id "${TOKEN_ID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

issue_payload="$("${BROKER_ADMIN}" --timeout-seconds 120 enrollment-token-issue \
  --id "${TOKEN_ID}" \
  --description "live agentd runtime activity proof")"
TOKEN_SECRET="$(printf '%s' "${issue_payload}" | json_field token.shared_secret)"

start_launcher() {
  local log_path="$1"
  "${ROOT}/tools/run_agentd_codexw_compat.sh" \
    --broker-mode native \
    --broker-url "${BROKER_URL}" \
    --deployment-id "${DEPLOYMENT_ID}" \
    --agentd-bin "${AGENTD_BIN}" \
    --agentd-port "${AGENTD_PORT}" \
    --agentd-auth-token "${AGENTD_TOKEN}" \
    --state-dir "${STATE_DIR}" \
    --db-path "${DB_PATH}" \
    --native-identity-dir "${IDENTITY_DIR}" \
    "${@:2}" \
    --native-no-reconnect \
    >"${log_path}" 2>&1 &
  LAUNCHER_PID=$!
}

start_launcher "${LAUNCHER_LOG}.bootstrap" \
  --native-enrollment-token-id "${TOKEN_ID}" \
  --native-enrollment-secret "${TOKEN_SECRET}"

for _ in $(seq 1 120); do
  if [[ -f "${IDENTITY_DIR}/deployment.cert.pem" ]]; then
    break
  fi
  sleep 0.25
done
if [[ ! -f "${IDENTITY_DIR}/deployment.cert.pem" ]]; then
  echo "connector did not enroll a deployment certificate; log: ${LAUNCHER_LOG}.bootstrap" >&2
  exit 1
fi

for _ in $(seq 1 60); do
  if "${BROKER_ADMIN}" --timeout-seconds 120 deployment-approve --deployment-id "${DEPLOYMENT_ID}" >/dev/null 2>&1; then
    break
  fi
  sleep 0.5
done
"${BROKER_ADMIN}" --timeout-seconds 120 deployment-approve --deployment-id "${DEPLOYMENT_ID}" >/dev/null 2>&1 || true
cleanup_launcher

start_launcher "${LAUNCHER_LOG}"

for _ in $(seq 1 120); do
  if curl -fsS --noproxy "*" -H "Authorization: Bearer ${AGENTD_TOKEN}" "${AGENTD_URL}/api/v1/health" >/dev/null 2>&1; then
    break
  fi
  sleep 0.25
done
curl -fsS --noproxy "*" -H "Authorization: Bearer ${AGENTD_TOKEN}" "${AGENTD_URL}/api/v1/health" >/dev/null

SESSION_ID="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${AGENTD_TOKEN}" \
  -H "Content-Type: application/json" \
  -d '{}' \
  "${AGENTD_URL}/api/v1/session/new" | json_field session_id)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${AGENTD_TOKEN}" \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
    "session_id": "${SESSION_ID}",
    "type": "codexw_live_agentd_activity_proof",
    "data": {
        "deployment_id": "${DEPLOYMENT_ID}",
        "purpose": "shared runtime-instance activity proof",
    },
    "append_to_session": False,
}))
PY
)" \
  "${AGENTD_URL}/api/v1/session/client_event" >/dev/null

python3 - <<PY
import importlib.machinery
import importlib.util
import json
import time
import urllib.parse
from pathlib import Path

broker_admin = "${BROKER_ADMIN}"
loader = importlib.machinery.SourceFileLoader("broker_admin", broker_admin)
spec = importlib.util.spec_from_loader(loader.name, loader)
mod = importlib.util.module_from_spec(spec)
loader.exec_module(mod)

args = mod.build_parser().parse_args(["me"])
mod.apply_local_defaults(args)
client = mod.BrokerAdminClient(
    base_url=args.base_url,
    server_id=args.server_id,
    client_id=args.client_id,
    shared_secret=args.shared_secret,
    timeout_seconds=120,
)
token = (args.token or "").strip() or client.login(args.username, mod.require_password(args.password))
deployment_id = "${DEPLOYMENT_ID}"
session_id = "${SESSION_ID}"

instance = None
last_payload = {}
deadline = time.time() + 60
while time.time() < deadline:
    payload = client.request("GET", "/api/v2/runtime-instances", token=token)
    last_payload = payload
    for candidate in payload.get("runtime_instances", []):
        if (
            candidate.get("runtime_kind") == "agentd"
            and candidate.get("placement", {}).get("deployment_id") == deployment_id
        ):
            instance = candidate
            break
    if instance and instance.get("connection", {}).get("state") == "online":
        break
    time.sleep(0.5)
if not instance:
    raise SystemExit("agentd runtime instance did not appear as agentd; last=" + json.dumps(last_payload)[:2000])
if instance.get("connection", {}).get("state") != "online":
    raise SystemExit("agentd runtime instance not online: " + json.dumps(instance)[:2000])

instance_id = instance.get("instance_id") or instance.get("runtime_instance_id")
path_id = urllib.parse.quote(instance_id, safe="")
caps = client.request("GET", f"/api/v2/runtime-instances/{path_id}/capabilities", token=token)
cap_text = json.dumps(caps.get("capabilities", {}), sort_keys=True)
if "sessions" not in cap_text or "events" not in cap_text:
    raise SystemExit("agentd capability response missing sessions/events surfaces: " + cap_text[:2000])

sessions = None
for _ in range(30):
    sessions = client.request("GET", f"/api/v2/runtime-instances/{path_id}/sessions", token=token)
    if any(s.get("session_id") == session_id for s in sessions.get("sessions", [])):
        break
    time.sleep(0.5)
if not sessions or not any(s.get("session_id") == session_id for s in sessions.get("sessions", [])):
    raise SystemExit("broker sessions route did not expose created agentd session: " + json.dumps(sessions)[:2000])

events = None
quoted_session_id = urllib.parse.quote(session_id, safe="")
for _ in range(30):
    events = client.request(
        "GET",
        f"/api/v2/runtime-instances/{path_id}/events?limit=8&session_id={quoted_session_id}",
        token=token,
    )
    if any(e.get("event") == "codexw_live_agentd_activity_proof" for e in events.get("events", [])):
        break
    time.sleep(0.5)
if not events or not any(e.get("event") == "codexw_live_agentd_activity_proof" for e in events.get("events", [])):
    raise SystemExit("broker events route did not expose created agentd event: " + json.dumps(events)[:2000])

proof = {
    "ok": True,
    "deployment_id": deployment_id,
    "runtime_instance_id": instance_id,
    "runtime_kind": instance.get("runtime_kind"),
    "connection_state": instance.get("connection", {}).get("state"),
    "session_id": session_id,
    "sessions_returned": len(sessions.get("sessions", [])),
    "events_returned": len(events.get("events", [])),
    "event_names": [e.get("event") for e in events.get("events", [])],
    "capability_has_sessions": "sessions" in cap_text,
    "capability_has_events": "events" in cap_text,
}
Path("${PROOF_JSON}").write_text(json.dumps(proof, indent=2, sort_keys=True) + "\\n", encoding="utf-8")
print(json.dumps(proof, indent=2, sort_keys=True))
PY

echo "${NAME} OK"
