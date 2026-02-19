#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

ROOT="$(agentd_smoke_project_root)"
LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

# shellcheck source=tools/lib/docker_preflight.sh
source "${ROOT}/tools/lib/docker_preflight.sh"
if ! docker_compose_preflight "broker-team-runs-quorum"; then
  exit 77
fi

PORT_STUB="$(agentd_smoke_pick_port)"
STUB_HOST="127.0.0.1"
STUB_BASE_CONTAINER="http://host.docker.internal:${PORT_STUB}/v1"
CURL_BASE_OPTS=(--max-time 30 --connect-timeout 5)

if [[ -z "${BROKER_PUBLISHED_PORT:-}" ]]; then
  BROKER_PUBLISHED_PORT=""
fi
if [[ -z "${KEYCLOAK_PUBLISHED_PORT:-}" ]]; then
  KEYCLOAK_PUBLISHED_PORT=""
fi
if [[ -z "${POSTGRES_PUBLISHED_PORT:-}" ]]; then
  POSTGRES_PUBLISHED_PORT=""
fi
if [[ -z "${AGENTD_PUBLISHED_PORT:-}" ]]; then
  AGENTD_PUBLISHED_PORT=""
fi
if [[ -z "${WEBUI_PUBLISHED_PORT:-}" ]]; then
  WEBUI_PUBLISHED_PORT=""
fi

cleanup() {
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

python3 -u - <<PY > "${LOG_DIR}/broker_team_runs_quorum_stub.stdout.log" 2> "${LOG_DIR}/broker_team_runs_quorum_stub.stderr.log" &
import json
from http.server import BaseHTTPRequestHandler, HTTPServer

class H(BaseHTTPRequestHandler):
  def log_message(self, fmt, *args):
    return

  def do_POST(self):
    if self.path != "/v1/chat/completions":
      self.send_response(404)
      self.end_headers()
      return
    body = {
      "id": "cmpl_stub",
      "object": "chat.completion",
      "created": 0,
      "model": "stub",
      "choices": [
        {"index": 0, "message": {"role": "assistant", "content": "OK"}, "finish_reason": "stop"}
      ],
    }
    data = json.dumps(body).encode("utf-8")
    self.send_response(200)
    self.send_header("Content-Type", "application/json; charset=utf-8")
    self.send_header("Content-Length", str(len(data)))
    self.end_headers()
    self.wfile.write(data)

HTTPServer(("127.0.0.1", ${PORT_STUB}), H).serve_forever()
PY
STUB_PID=$!

detect_running_stack() {
  python3 -u - <<'PY'
import json
import re
import subprocess
import sys
import urllib.request

def parse_labels(raw: str) -> dict:
  out = {}
  for item in (raw or "").split(","):
    if "=" in item:
      k, v = item.split("=", 1)
      out[k.strip()] = v.strip()
  return out

def port_for(ports: str) -> str:
  m = re.search(r":(\d+)->", ports or "")
  return m.group(1) if m else ""

def is_healthy(obj: dict) -> bool:
  status = (obj.get("Status") or "").lower()
  if "unhealthy" in status:
    return False
  return True

def oidc_ready(port: str) -> bool:
  if not port:
    return False
  url = f"http://127.0.0.1:{port}/realms/agentd/.well-known/openid-configuration"
  try:
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(url, timeout=2) as resp:
      if resp.status != 200:
        return False
      payload = resp.read(2048)
    return b'"issuer"' in payload
  except Exception:
    return False

lines = subprocess.check_output(["docker", "ps", "--format", "{{json .}}"], text=True).splitlines()
projects = {}
for line in lines:
  try:
    obj = json.loads(line)
  except json.JSONDecodeError:
    continue
  labels = parse_labels(obj.get("Labels", ""))
  project = labels.get("com.docker.compose.project", "")
  service = labels.get("com.docker.compose.service", "")
  if not project or not service:
    continue
  projects.setdefault(project, {})[service] = obj

for project, services in projects.items():
  if not {"broker", "keycloak", "agentd", "webui"}.issubset(services):
    continue
  if not all(is_healthy(services[name]) for name in ("broker", "keycloak", "agentd", "webui")):
    continue
  broker_port = port_for(services["broker"].get("Ports", ""))
  keycloak_port = port_for(services["keycloak"].get("Ports", ""))
  agentd_port = port_for(services["agentd"].get("Ports", ""))
  webui_port = port_for(services["webui"].get("Ports", ""))
  if not (broker_port and keycloak_port and agentd_port and webui_port):
    continue
  if not oidc_ready(keycloak_port):
    continue
  print(f"COMPOSE_PROJECT_NAME={project}")
  print(f"BROKER_PUBLISHED_PORT={broker_port}")
  print(f"KEYCLOAK_PUBLISHED_PORT={keycloak_port}")
  print(f"AGENTD_PUBLISHED_PORT={agentd_port}")
  print(f"WEBUI_PUBLISHED_PORT={webui_port}")
  sys.stdout.flush()
  sys.exit(0)
PY
}

if [[ -n "${BROKER_PUBLISHED_PORT}" && -n "${KEYCLOAK_PUBLISHED_PORT}" && -n "${AGENTD_PUBLISHED_PORT}" && -n "${WEBUI_PUBLISHED_PORT}" ]]; then
  STACK_ENV=""
else
  STACK_ENV="$(detect_running_stack || true)"
fi

if [[ -n "${STACK_ENV}" ]]; then
  eval "${STACK_ENV}"
elif [[ -n "${BROKER_PUBLISHED_PORT}" && -n "${KEYCLOAK_PUBLISHED_PORT}" && -n "${AGENTD_PUBLISHED_PORT}" && -n "${WEBUI_PUBLISHED_PORT}" ]]; then
  :
else
  BROKER_PUBLISHED_PORT="$(agentd_smoke_pick_port)"
  KEYCLOAK_PUBLISHED_PORT="$(agentd_smoke_pick_port)"
  POSTGRES_PUBLISHED_PORT="$(agentd_smoke_pick_port)"
  AGENTD_PUBLISHED_PORT="$(agentd_smoke_pick_port)"
  WEBUI_PUBLISHED_PORT="$(agentd_smoke_pick_port)"
  export BROKER_PUBLISHED_PORT KEYCLOAK_PUBLISHED_PORT POSTGRES_PUBLISHED_PORT AGENTD_PUBLISHED_PORT WEBUI_PUBLISHED_PORT

  VERIFY_LOG="${LOG_DIR}/broker_team_runs_quorum_compose_verify.log"
  if ! "${ROOT}/tools/verify_compose_stack.sh" > "${VERIFY_LOG}" 2>&1; then
    cat "${VERIFY_LOG}" >&2 || true
    if [[ -n "${COMPOSE_PROJECT_NAME:-}" ]]; then
      (cd "${ROOT}" && docker compose -p "${COMPOSE_PROJECT_NAME}" down -v --remove-orphans >/dev/null 2>&1) || true
    fi
    exit 1
  fi
fi

BROKER_BASE="https://127.0.0.1:${BROKER_PUBLISHED_PORT}"
KEYCLOAK_BASE="http://keycloak.lvh.me:${KEYCLOAK_PUBLISHED_PORT}"

get_token() {
  local token_json
  token_json="$(
    curl -fsS --noproxy "*" "${CURL_BASE_OPTS[@]}" \
      -d "grant_type=password" \
      -d "client_id=agentd-broker-dev" \
      -d "username=test" \
      -d "password=test" \
      "${KEYCLOAK_BASE}/realms/agentd/protocol/openid-connect/token"
  )"
  echo "${token_json}" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("access_token",""))'
}

OIDC_JWT="$(get_token)"
if [[ -z "${OIDC_JWT}" ]]; then
  echo "failed to fetch OIDC token" >&2
  exit 2
fi

TEAM_JSON="$(
  curl -fsS -k --noproxy "*" "${CURL_BASE_OPTS[@]}" \
    -H "Authorization: Bearer ${OIDC_JWT}" \
    -H "Content-Type: application/json" \
    -d '{"display_name":"Team Quorum Smoke"}' \
    "${BROKER_BASE}/v1/teams"
)"
TEAM_ID="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${TEAM_JSON}''')
team = obj.get("team") or {}
print(team.get("team_id",""))
PY
)"
if [[ -z "${TEAM_ID}" ]]; then
  echo "failed to create team: ${TEAM_JSON}" >&2
  exit 1
fi

MEMBER_PLANNER="m_planner_${TEAM_ID}"
MEMBER_REVIEWER="m_reviewer_${TEAM_ID}"

curl -fsS -k --noproxy "*" "${CURL_BASE_OPTS[@]}" \
  -H "Authorization: Bearer ${OIDC_JWT}" \
  -H "Content-Type: application/json" \
  -d "{\"member_id\":\"${MEMBER_PLANNER}\",\"agent_id\":\"agent1\",\"role\":\"planner\",\"status\":\"active\"}" \
  "${BROKER_BASE}/v1/teams/${TEAM_ID}/members" >/dev/null

curl -fsS -k --noproxy "*" "${CURL_BASE_OPTS[@]}" \
  -H "Authorization: Bearer ${OIDC_JWT}" \
  -H "Content-Type: application/json" \
  -d "{\"member_id\":\"${MEMBER_REVIEWER}\",\"agent_id\":\"agent1\",\"role\":\"reviewer\",\"status\":\"active\"}" \
  "${BROKER_BASE}/v1/teams/${TEAM_ID}/members" >/dev/null

QUORUM_RULE="$(python3 - <<PY
import json
rule = {
  "action": "team_run",
  "tool_names": [],
  "min_approvals": 2,
  "role_allowlist": ["planner", "reviewer"],
  "require_distinct_roles": True,
  "timeout_ms": 30000,
  "quorum_mode": "strict",
}
print(json.dumps(rule))
PY
)"

curl -fsS -k --noproxy "*" "${CURL_BASE_OPTS[@]}" \
  -H "Authorization: Bearer ${OIDC_JWT}" \
  -H "Content-Type: application/json" \
  -d "${QUORUM_RULE}" \
  "${BROKER_BASE}/v1/teams/${TEAM_ID}/quorum" >/dev/null

RUN_BODY_NO_APPROVALS="$(python3 - <<PY
import json
run = {
  "prompt": "Return exactly: OK",
  "no_session": True,
  "tools": "none",
  "base_url": "${STUB_BASE_CONTAINER}",
  "api_key": "dummy",
  "model": "stub",
  "trace": False,
}
team = {"quorum_policy": {"mode": "auto"}}
print(json.dumps({"run": run, "team": team}))
PY
)"

NO_APPROVAL_PATH="${LOG_DIR}/broker_team_runs_quorum_no_approval.json"
NO_APPROVAL_CODE="$(
  curl -sS -k --noproxy "*" "${CURL_BASE_OPTS[@]}" \
    -H "Authorization: Bearer ${OIDC_JWT}" \
    -H "Content-Type: application/json" \
    -d "${RUN_BODY_NO_APPROVALS}" \
    -o "${NO_APPROVAL_PATH}" \
    -w "%{http_code}" \
    "${BROKER_BASE}/v1/teams/${TEAM_ID}/runs"
)"
if [[ "${NO_APPROVAL_CODE}" != "409" ]]; then
  echo "expected 409 for missing quorum approvals, got ${NO_APPROVAL_CODE}" >&2
  cat "${NO_APPROVAL_PATH}" >&2 || true
  exit 1
fi

python3 - <<PY
import json, sys
obj = json.load(open("${NO_APPROVAL_PATH}"))
quorum = obj.get("quorum") or {}
rules = quorum.get("rules") or []
if not rules:
  print("expected quorum rules in response", obj, file=sys.stderr)
  raise SystemExit(1)
PY

RUN_BODY_APPROVED="$(python3 - <<PY
import json
run = {
  "prompt": "Return exactly: OK",
  "no_session": True,
  "tools": "none",
  "base_url": "${STUB_BASE_CONTAINER}",
  "api_key": "dummy",
  "model": "stub",
  "trace": False,
}
team = {
  "quorum_policy": {"mode": "auto"},
  "approvals": [
    {"member_id": "${MEMBER_PLANNER}", "decision": "approve"},
    {"member_id": "${MEMBER_REVIEWER}", "decision": "approve"},
  ],
}
print(json.dumps({"run": run, "team": team}))
PY
)"

RUN_JSON="$(
  curl -fsS -k --noproxy "*" "${CURL_BASE_OPTS[@]}" \
    -H "Authorization: Bearer ${OIDC_JWT}" \
    -H "Content-Type: application/json" \
    -d "${RUN_BODY_APPROVED}" \
    "${BROKER_BASE}/v1/teams/${TEAM_ID}/runs"
)"
TEAM_RUN_ID="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${RUN_JSON}''')
print(obj.get("team_run_id",""))
PY
)"
RUN_STATUS="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${RUN_JSON}''')
print(obj.get("status",""))
PY
)"
if [[ -z "${TEAM_RUN_ID}" || "${RUN_STATUS}" != "succeeded" ]]; then
  echo "unexpected team run result: ${RUN_JSON}" >&2
  exit 1
fi

APPROVALS_JSON="$(
  curl -fsS -k --noproxy "*" "${CURL_BASE_OPTS[@]}" \
    -H "Authorization: Bearer ${OIDC_JWT}" \
    "${BROKER_BASE}/v1/teams/${TEAM_ID}/runs/${TEAM_RUN_ID}/approvals"
)"
python3 - <<PY
import json, sys
obj = json.loads(r'''${APPROVALS_JSON}''')
approvals = obj.get("approvals") or []
if len(approvals) < 2:
  print("expected approvals to be persisted", obj, file=sys.stderr)
  raise SystemExit(1)
PY

echo "broker_team_runs_quorum_compose_smoke OK"
