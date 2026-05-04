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

python3 - <<PY
import importlib.machinery
import importlib.util
import base64
import json
import time
import urllib.parse
import urllib.request
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
agentd_url = "${AGENTD_URL}"
agentd_token = "${AGENTD_TOKEN}"

instance = None
last_payload = {}
preferred_instance_id = f"{deployment_id}-runtime"
deadline = time.time() + 60
while time.time() < deadline:
    payload = client.request("GET", "/api/v2/runtime-instances", token=token)
    last_payload = payload
    candidates = []
    for candidate in payload.get("runtime_instances", []):
        if (
            candidate.get("runtime_kind") == "agentd"
            and candidate.get("placement", {}).get("deployment_id") == deployment_id
        ):
            candidates.append(candidate)
    online_candidates = [c for c in candidates if c.get("connection", {}).get("state") == "online"]
    exact_candidates = [
        c
        for c in online_candidates
        if (c.get("instance_id") or c.get("runtime_instance_id")) == preferred_instance_id
    ]
    if exact_candidates:
        instance = exact_candidates[0]
        break
    if online_candidates:
        instance = online_candidates[0]
        break
    if candidates:
        instance = candidates[0]
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
for expected in ("sessions", "session_create", "events", "status", "proxy_http"):
    if expected not in cap_text:
        raise SystemExit(f"agentd capability response missing {expected} surface: " + cap_text[:2000])
for expected in ("files", "shell"):
    if expected not in cap_text:
        raise SystemExit(f"agentd capability response missing {expected} iOS inspection surface: " + cap_text[:2000])
for expected in ("voice_webrtc_peer", "voice.webrtc_peer.status"):
    if expected not in cap_text:
        raise SystemExit(f"agentd capability response missing {expected} media-runtime action surface: " + cap_text[:2000])

created = client.request(
    "POST",
    f"/api/v2/runtime-instances/{path_id}/sessions",
    {
        "objective": "codexw live agentd activity proof",
        "client_id": "codexw-live-agentd-activity-proof",
    },
    token=token,
)
session_id = created.get("session", {}).get("session_id")
if not session_id:
    raise SystemExit("broker session-create route did not return a session_id: " + json.dumps(created)[:2000])

proxy_status = client.request(
    "POST",
    f"/api/v2/runtime-instances/{path_id}/proxy/http",
    {"method": "GET", "path": "/api/v1/runtime/status"},
    token=token,
)
proxy_text = json.dumps(proxy_status, sort_keys=True)
if not proxy_status.get("ok") or "agentd" not in proxy_text:
    raise SystemExit("broker HTTP proxy did not expose agentd runtime status: " + proxy_text[:2000])

voice_status = client.request(
    "POST",
    f"/api/v2/runtime-instances/{path_id}/actions",
    {"action": "voice.webrtc_peer.status", "input": {"session_id": session_id}},
    token=token,
)
voice_text = json.dumps(voice_status, sort_keys=True)
if not voice_status.get("ok") or "voice.webrtc_peer.status" not in voice_text:
    raise SystemExit("broker runtime action did not expose agentd voice peer status: " + voice_text[:2000])

deployment_path = urllib.parse.quote(deployment_id, safe="")
files = client.request(
    "POST",
    f"/api/v1/deployments/{deployment_path}/files/list",
    {"path": ".", "limit": 40},
    token=token,
)
entries = files.get("entries", []) if isinstance(files, dict) else []
if not files.get("ok") or not entries:
    raise SystemExit("broker file-list route did not expose agentd file explorer entries: " + json.dumps(files)[:2000])
read_entry = None
for candidate in entries:
    if isinstance(candidate, dict) and not candidate.get("is_directory") and candidate.get("name") == "README.md":
        read_entry = candidate
        break
if read_entry is None:
    for candidate in entries:
        if isinstance(candidate, dict) and not candidate.get("is_directory") and candidate.get("path"):
            read_entry = candidate
            break
if read_entry is None:
    raise SystemExit("broker file-list route returned no readable agentd file entries: " + json.dumps(files)[:2000])
file_read = client.request(
    "POST",
    f"/api/v1/deployments/{deployment_path}/files/read",
    {"path": read_entry.get("path"), "limit": 4096},
    token=token,
)
file_bytes = base64.b64decode(file_read.get("data_base64") or b"", validate=True)
if not file_read.get("ok") or not file_bytes:
    raise SystemExit("broker file-read route did not return readable agentd file content: " + json.dumps(file_read)[:2000])

shell_start = client.request(
    "POST",
    f"/api/v1/deployments/{deployment_path}/shells/start",
    {
        "command": "printf agentd-shell-proof",
        "label": "codexw live agentd shell proof",
        "intent": "observation",
    },
    token=token,
)
shell = shell_start.get("shell") if isinstance(shell_start, dict) else None
if not isinstance(shell, dict) and isinstance(shell_start.get("result") if isinstance(shell_start, dict) else None, dict):
    shell = shell_start["result"].get("shell")
shell_id = shell.get("id") if isinstance(shell, dict) else None
if not shell_id:
    raise SystemExit("broker shell-start route did not return an agentd shell id: " + json.dumps(shell_start)[:2000])
shell_path = urllib.parse.quote(str(shell_id), safe="")
shell_read = None
for _ in range(20):
    shell_read = client.request("GET", f"/api/v1/deployments/{deployment_path}/shells/{shell_path}", token=token)
    shell = shell_read.get("shell") if isinstance(shell_read, dict) else None
    lines = []
    if isinstance(shell, dict):
        lines = shell.get("recent_lines") or shell.get("output_lines") or []
    if any("agentd-shell-proof" in str(line) for line in lines):
        break
    time.sleep(0.25)
if not shell_read:
    raise SystemExit("broker shell-read route returned no response for agentd shell")
shell = shell_read.get("shell") if isinstance(shell_read, dict) else None
lines = shell.get("recent_lines") or shell.get("output_lines") or [] if isinstance(shell, dict) else []
if not isinstance(shell, dict) or not any("agentd-shell-proof" in str(line) for line in lines):
    raise SystemExit("broker shell-read route did not expose agentd shell output: " + json.dumps(shell_read)[:2000])

event_body = json.dumps({
    "session_id": session_id,
    "type": "codexw_live_agentd_activity_proof",
    "data": {
        "deployment_id": deployment_id,
        "purpose": "shared runtime-instance activity proof",
    },
    "append_to_session": False,
}).encode()
event_request = urllib.request.Request(
    agentd_url.rstrip("/") + "/api/v1/session/client_event",
    data=event_body,
    headers={
        "Authorization": f"Bearer {agentd_token}",
        "Content-Type": "application/json",
        "Accept": "application/json",
    },
    method="POST",
)
with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(event_request, timeout=10) as response:
    response.read()

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
    "capability_has_session_create": "session_create" in cap_text,
    "capability_has_events": "events" in cap_text,
    "capability_has_proxy_http": "proxy_http" in cap_text,
    "capability_has_files": "files" in cap_text,
    "capability_has_shell": "shell" in cap_text,
    "capability_has_voice_webrtc_peer": "voice_webrtc_peer" in cap_text,
    "capability_has_voice_webrtc_peer_status_action": "voice.webrtc_peer.status" in cap_text,
    "proxy_status_ok": bool(proxy_status.get("ok")),
    "voice_peer_status_ok": bool(voice_status.get("ok")),
    "file_list_entries": len(entries),
    "file_read_path": file_read.get("path"),
    "file_read_bytes": len(file_bytes),
    "shell_id": shell_id,
    "shell_status": shell.get("status") if isinstance(shell, dict) else None,
    "shell_output_proved": any("agentd-shell-proof" in str(line) for line in lines),
}
Path("${PROOF_JSON}").write_text(json.dumps(proof, indent=2, sort_keys=True) + "\\n", encoding="utf-8")
print(json.dumps(proof, indent=2, sort_keys=True))
PY

echo "${NAME} OK"
