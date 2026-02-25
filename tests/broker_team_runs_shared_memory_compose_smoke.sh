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
if ! docker_compose_preflight "broker-team-shared-memory"; then
  exit 77
fi

PORT_STUB="$(agentd_smoke_pick_port)"
STUB_HOST="127.0.0.1"
STUB_BASE_HOST="http://${STUB_HOST}:${PORT_STUB}/v1"
STUB_BASE_CONTAINER="http://host.docker.internal:${PORT_STUB}/v1"
CURL_BASE_OPTS=(-q --max-time 30 --connect-timeout 5)

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
if [[ "${AGENT_SMOKE_USE_PUBLISHED_PORTS:-}" != "1" ]]; then
  BROKER_PUBLISHED_PORT=""
  KEYCLOAK_PUBLISHED_PORT=""
  POSTGRES_PUBLISHED_PORT=""
  AGENTD_PUBLISHED_PORT=""
  WEBUI_PUBLISHED_PORT=""
fi
STACK_FROM_DETECT="0"
STACK_STARTED="0"

cleanup() {
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
  if [[ "${STACK_STARTED}" == "1" && -n "${COMPOSE_PROJECT_NAME:-}" ]]; then
    (cd "${ROOT}" && docker compose -p "${COMPOSE_PROJECT_NAME}" down -v --remove-orphans >/dev/null 2>&1) || true
  fi
}
trap cleanup EXIT

# OpenAI-compatible stub: emits a memory_write tool call, then OK.
python3 -u - <<PY > "${LOG_DIR}/broker_team_runs_shared_memory_stub.stdout.log" 2> "${LOG_DIR}/broker_team_runs_shared_memory_stub.stderr.log" &
import json
from http.server import BaseHTTPRequestHandler, HTTPServer

def has_tool_result(messages):
  for m in messages:
    if not isinstance(m, dict):
      continue
    if m.get("role") == "tool":
      return True
  return False

class H(BaseHTTPRequestHandler):
  def log_message(self, fmt, *args):
    return

  def do_POST(self):
    if self.path != "/v1/chat/completions":
      self.send_response(404)
      self.end_headers()
      return
    try:
      length = int(self.headers.get("Content-Length", "0"))
    except ValueError:
      length = 0
    raw = self.rfile.read(length) if length > 0 else b"{}"
    try:
      req = json.loads(raw.decode("utf-8"))
    except Exception:
      req = {}
    messages = req.get("messages") if isinstance(req.get("messages"), list) else []

    if has_tool_result(messages):
      body = {
        "id": "cmpl_stub_2",
        "object": "chat.completion",
        "created": 0,
        "model": "stub",
        "choices": [
          {"index": 0, "message": {"role": "assistant", "content": "OK"}, "finish_reason": "stop"}
        ],
      }
    else:
      body = {
        "id": "cmpl_stub_1",
        "object": "chat.completion",
        "created": 0,
        "model": "stub",
        "choices": [
          {
            "index": 0,
            "message": {
              "role": "assistant",
              "content": "",
              "tool_calls": [
                {
                  "id": "call_1",
                  "type": "function",
                  "function": {
                    "name": "memory_write",
                    "arguments": json.dumps({"text": "shared-memory-smoke", "layer": "daily"}),
                  },
                }
              ],
            },
            "finish_reason": "tool_calls",
          }
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
  m = re.search(r":(\\d+)->", ports or "")
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

if [[ "${BROKER_SMOKE_FORCE_NEW_STACK:-}" == "1" ]]; then
  STACK_ENV=""
elif [[ -n "${BROKER_PUBLISHED_PORT}" && -n "${KEYCLOAK_PUBLISHED_PORT}" && -n "${AGENTD_PUBLISHED_PORT}" && -n "${WEBUI_PUBLISHED_PORT}" ]]; then
  STACK_ENV=""
else
  STACK_ENV="$(detect_running_stack || true)"
fi

start_compose_stack() {
  BROKER_PUBLISHED_PORT="$(agentd_smoke_pick_port)"
  KEYCLOAK_PUBLISHED_PORT="$(agentd_smoke_pick_port)"
  POSTGRES_PUBLISHED_PORT="$(agentd_smoke_pick_port)"
  AGENTD_PUBLISHED_PORT="$(agentd_smoke_pick_port)"
  WEBUI_PUBLISHED_PORT="$(agentd_smoke_pick_port)"
  export BROKER_PUBLISHED_PORT KEYCLOAK_PUBLISHED_PORT POSTGRES_PUBLISHED_PORT AGENTD_PUBLISHED_PORT WEBUI_PUBLISHED_PORT

  VERIFY_LOG="${LOG_DIR}/broker_team_runs_shared_memory_compose_verify.log"
  "${ROOT}/tools/verify_compose_stack.sh" > "${VERIFY_LOG}" 2>&1
  verify_status=$?
  if [[ "${verify_status}" -ne 0 ]]; then
    cat "${VERIFY_LOG}" >&2 || true
    if [[ "${verify_status}" -eq 77 ]]; then
      exit 77
    fi
    if [[ -n "${COMPOSE_PROJECT_NAME:-}" ]]; then
      (cd "${ROOT}" && docker compose -p "${COMPOSE_PROJECT_NAME}" down -v --remove-orphans >/dev/null 2>&1) || true
    fi
    exit 1
  fi
  STACK_STARTED="1"
}

if [[ -n "${STACK_ENV}" ]]; then
  eval "${STACK_ENV}"
  STACK_FROM_DETECT="1"
elif [[ -n "${BROKER_PUBLISHED_PORT}" && -n "${KEYCLOAK_PUBLISHED_PORT}" && -n "${AGENTD_PUBLISHED_PORT}" && -n "${WEBUI_PUBLISHED_PORT}" ]]; then
  :
else
  start_compose_stack
fi

BROKER_BASE="https://127.0.0.1:${BROKER_PUBLISHED_PORT}"
KEYCLOAK_BASE="http://keycloak.lvh.me:${KEYCLOAK_PUBLISHED_PORT}"
AGENTD_BASE="http://127.0.0.1:${AGENTD_PUBLISHED_PORT}"

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

select_agent_id() {
  local resp code body
  resp="$(
    curl -sS -k --noproxy "*" "${CURL_BASE_OPTS[@]}" \
      -H "Authorization: Bearer ${OIDC_JWT}" \
      -w "\n%{http_code}" \
      "${BROKER_BASE}/v1/agents" || true
  )"
  code="${resp##*$'\n'}"
  body="${resp%$'\n'*}"
  if [[ "${code}" != "200" || -z "${body}" ]]; then
    return 1
  fi
  python3 - "${body}" <<'PY'
import json, sys
raw = sys.argv[1] if len(sys.argv) > 1 else ""
obj = json.loads(raw or "{}")
agents = obj.get("agents") or []
for a in agents:
  if a.get("connected") is True and a.get("agent_id"):
    print(a["agent_id"])
    raise SystemExit(0)
for a in agents:
  if a.get("agent_id"):
    print(a["agent_id"])
    raise SystemExit(0)
raise SystemExit(1)
PY
}

ensure_team_agent() {
  TEAM_AGENT_ID="$(select_agent_id || true)"
  if [[ -n "${TEAM_AGENT_ID}" ]]; then
    return 0
  fi
  curl -sS -k --noproxy "*" "${CURL_BASE_OPTS[@]}" \
    -H "Authorization: Bearer ${OIDC_JWT}" \
    -H "Content-Type: application/json" \
    -d '{"agent_id":"agent1"}' \
    -o /dev/null \
    "${BROKER_BASE}/v1/agents" || true
  TEAM_AGENT_ID="$(select_agent_id || true)"
  if [[ -n "${TEAM_AGENT_ID}" ]]; then
    return 0
  fi
  return 1
}

wait_for_team_agent() {
  local deadline now
  deadline="$(( $(date +%s) + ${AGENT_SMOKE_AGENT_WAIT_SECS:-60} ))"
  while true; do
    if ensure_team_agent; then
      return 0
    fi
    now="$(date +%s)"
    if (( now >= deadline )); then
      return 1
    fi
    sleep 2
  done
}

TEAM_AGENT_ID=""
if ! wait_for_team_agent; then
  if [[ "${STACK_FROM_DETECT}" == "1" ]]; then
    echo "SKIP: no accessible agents in detected broker stack" >&2
    exit 77
  else
    echo "no accessible agents for current OIDC user" >&2
    exit 1
  fi
fi

TEAM_JSON="$(
  curl -fsS -k --noproxy "*" "${CURL_BASE_OPTS[@]}" \
    -H "Authorization: Bearer ${OIDC_JWT}" \
    -H "Content-Type: application/json" \
    -d '{"display_name":"Team Runs Shared Memory Smoke"}' \
    "${BROKER_BASE}/v1/teams"
)"
TEAM_ID="$(python3 - <<PY
import json
obj = json.loads(r'''${TEAM_JSON}''')
team = obj.get("team") or {}
print(team.get("team_id",""))
PY
)"
if [[ -z "${TEAM_ID}" ]]; then
  echo "failed to create team: ${TEAM_JSON}" >&2
  exit 1
fi

curl -fsS -k --noproxy "*" "${CURL_BASE_OPTS[@]}" \
  -H "Authorization: Bearer ${OIDC_JWT}" \
  -H "Content-Type: application/json" \
  -d "{\"member_id\":\"m_planner_${TEAM_ID}\",\"agent_id\":\"${TEAM_AGENT_ID}\",\"role\":\"planner\",\"status\":\"active\"}" \
  "${BROKER_BASE}/v1/teams/${TEAM_ID}/members" >/dev/null

SCOPE_ID="shared_mem_${TEAM_ID}_$(date +%s)"
RUN_BODY="$(python3 - <<PY
import json
run = {
  "prompt": "Trigger memory_write tool call",
  "no_session": True,
  "tools": "host",
  "base_url": "${STUB_BASE_CONTAINER}",
  "api_key": "dummy",
  "model": "stub",
  "verbose": True,
  "require_tool_call": True,
  "max_steps": 4,
}
team = {
  "mode": "async",
  "role": "planner",
  "shared_memory_scope_id": "${SCOPE_ID}",
  "shared_memory_mode": "read_only",
}
print(json.dumps({"run": run, "team": team}))
PY
)"

RUN_JSON="$(
  curl -fsS -k --noproxy "*" "${CURL_BASE_OPTS[@]}" \
    -H "Authorization: Bearer ${OIDC_JWT}" \
    -H "Content-Type: application/json" \
    -d "${RUN_BODY}" \
    "${BROKER_BASE}/v1/teams/${TEAM_ID}/runs"
)"
TEAM_RUN_ID="$(python3 - <<PY
import json
obj = json.loads(r'''${RUN_JSON}''')
print(obj.get("team_run_id",""))
PY
)"
JOB_ID="$(python3 - <<PY
import json
obj = json.loads(r'''${RUN_JSON}''')
items = obj.get("member_jobs") or []
job_id = ""
if items:
  job_id = (items[0] or {}).get("job_id") or ""
print(job_id)
PY
)"
if [[ -z "${TEAM_RUN_ID}" || -z "${JOB_ID}" ]]; then
  echo "unexpected team run response: ${RUN_JSON}" >&2
  exit 1
fi

STATUS_JSON="$(
  curl -fsS -k --noproxy "*" "${CURL_BASE_OPTS[@]}" \
    -H "Authorization: Bearer ${OIDC_JWT}" \
    "${BROKER_BASE}/v1/teams/${TEAM_ID}/runs/${TEAM_RUN_ID}"
)"
python3 - <<PY
import json, sys
obj = json.loads(r'''${STATUS_JSON}''')
if obj.get("shared_memory_scope_id") != "${SCOPE_ID}":
  print("missing shared_memory_scope_id", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("shared_memory_mode") != "read_only":
  print("missing shared_memory_mode", obj, file=sys.stderr)
  raise SystemExit(1)
PY

AUTH_HEADER=("Authorization: Bearer dev-agentd-token")

deadline=$(( $(date +%s) + 120 ))
while true; do
  now=$(date +%s)
  if [[ "${now}" -gt "${deadline}" ]]; then
    echo "timed out waiting for job ${JOB_ID}" >&2
    exit 1
  fi
  job_json="$(curl -fsS --noproxy "*" --max-time 10 \
    -H "${AUTH_HEADER[0]}" \
    "${AGENTD_BASE}/api/v1/job?job_id=$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote('${JOB_ID}'))
PY
)&include_events=1&max_events=256")"
  set +e
  python3 - <<PY
import json, sys
obj = json.loads(r'''${job_json}''')
if not obj.get("ok"):
  print("job query failed", obj, file=sys.stderr)
  raise SystemExit(1)
status = obj.get("status")
if status not in ("done", "error", "cancelled"):
  raise SystemExit(2)
res = obj.get("result")
if not isinstance(res, dict):
  print("missing result", obj, file=sys.stderr)
  raise SystemExit(1)
if res.get("memory_scope_id") != "${SCOPE_ID}":
  print("missing memory_scope_id", res, file=sys.stderr)
  raise SystemExit(1)
if res.get("memory_scope_mode") != "read_only":
  print("missing memory_scope_mode", res, file=sys.stderr)
  raise SystemExit(1)
# Ensure tool_result includes read_only error.
needle = "memory scope is read_only"
found = False
for ev in res.get("events") or []:
  if not isinstance(ev, dict):
    continue
  if ev.get("type") != "tool_result":
    continue
  if needle in json.dumps(ev):
    found = True
    break
if not found:
  print("missing tool_result read_only error", res, file=sys.stderr)
  raise SystemExit(1)
raise SystemExit(0)
PY
  rc=$?
  set -e
  if [[ $rc -eq 0 ]]; then
    break
  elif [[ $rc -eq 1 ]]; then
    exit 1
  fi
  sleep 0.2
done

echo "broker_team_runs_shared_memory_compose_smoke OK (stub=${STUB_BASE_HOST})"
