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
if ! docker_compose_preflight "broker-orchestrator-loop"; then
  exit 77
fi

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
  if [[ "${STACK_STARTED}" == "1" && -n "${COMPOSE_PROJECT_NAME:-}" ]]; then
    (cd "${ROOT}" && docker compose -p "${COMPOSE_PROJECT_NAME}" down -v --remove-orphans >/dev/null 2>&1) || true
  fi
}
trap cleanup EXIT

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

  VERIFY_LOG="${LOG_DIR}/broker_orchestrator_loop_compose_verify.log"
  if ! "${ROOT}/tools/verify_compose_stack.sh" > "${VERIFY_LOG}" 2>&1; then
    cat "${VERIFY_LOG}" >&2 || true
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
    -d '{"display_name":"Orchestrator Loop Smoke","meta":{"role_graph":[{"from_role":"planner","to_role":"executor"}]}}' \
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

RUN_JSON="$(
  curl -fsS -k --noproxy "*" "${CURL_BASE_OPTS[@]}" \
    -H "Authorization: Bearer ${OIDC_JWT}" \
    -H "Content-Type: application/json" \
    -d '{"goal":"Loop smoke goal","status":"running","meta":{"team_mode":"async","spawn_missing_roles":true,"spawn_count_by_role":{"planner":2,"executor":1},"spawn_requirements":{"region":"us"},"spawn_requirements_by_role":{"planner":{"region":"eu","tier":"gpu"}}}}' \
    "${BROKER_BASE}/v1/teams/${TEAM_ID}/orchestrator/runs"
)"
RUN_ID="$(python3 - <<PY
import json
obj = json.loads(r'''${RUN_JSON}''')
run = obj.get("run") or {}
print(run.get("orchestrator_run_id",""))
PY
)"
if [[ -z "${RUN_ID}" ]]; then
  echo "failed to create orchestrator run: ${RUN_JSON}" >&2
  exit 1
fi

TAKEOVER_JSON="$(
  curl -fsS -k --noproxy "*" "${CURL_BASE_OPTS[@]}" \
    -H "Authorization: Bearer ${OIDC_JWT}" \
    -H "Content-Type: application/json" \
    -d '{"goal":"Takeover smoke goal","status":"running","meta":{"team_mode":"async","orchestrator_owner":"orch_prev","allow_takeover":true}}' \
    "${BROKER_BASE}/v1/teams/${TEAM_ID}/orchestrator/runs"
)"
TAKEOVER_ID="$(python3 - <<PY
import json
obj = json.loads(r'''${TAKEOVER_JSON}''')
run = obj.get("run") or {}
print(run.get("orchestrator_run_id",""))
PY
)"
if [[ -z "${TAKEOVER_ID}" ]]; then
  echo "failed to create takeover run: ${TAKEOVER_JSON}" >&2
  exit 1
fi

ORCH_LOG="${LOG_DIR}/broker_orchestrator_loop.log"
(
  cd "${ROOT}/broker"
  go run ./cmd/agentd-orchestrator \
    --broker-base "${BROKER_BASE}" \
    --oidc-token "${OIDC_JWT}" \
    --insecure \
    --orchestrator-id "orch_takeover" \
    --once
) > "${ORCH_LOG}" 2>&1 || {
  cat "${ORCH_LOG}" >&2 || true
  exit 1
}

SPAWN_LIST_JSON="$(
  curl -fsS -k --noproxy "*" "${CURL_BASE_OPTS[@]}" \
    -H "Authorization: Bearer ${OIDC_JWT}" \
    "${BROKER_BASE}/v1/teams/${TEAM_ID}/orchestrator/spawn_requests?orchestrator_run_id=${RUN_ID}&limit=50"
)"
python3 - <<PY
import json, sys
obj = json.loads(r'''${SPAWN_LIST_JSON}''')
items = obj.get("spawn_requests") or []
if not isinstance(items, list) or len(items) < 1:
  print("expected spawn requests", obj, file=sys.stderr)
  raise SystemExit(1)
by_role = {}
for item in items:
  if not isinstance(item, dict):
    continue
  role = str(item.get("role","")).strip().lower()
  if not role:
    continue
  by_role[role] = item
if "planner" not in by_role or "executor" not in by_role:
  print("expected spawn request roles planner/executor", obj, file=sys.stderr)
  raise SystemExit(1)
planner = by_role["planner"]
executor = by_role["executor"]
if int(planner.get("count") or 0) != 2:
  print("planner spawn count mismatch", planner, file=sys.stderr)
  raise SystemExit(1)
if int(executor.get("count") or 0) != 1:
  print("executor spawn count mismatch", executor, file=sys.stderr)
  raise SystemExit(1)
preq = planner.get("requirements") or {}
ereq = executor.get("requirements") or {}
if str(preq.get("region","")) != "eu" or str(preq.get("tier","")) != "gpu":
  print("planner requirements mismatch", planner, file=sys.stderr)
  raise SystemExit(1)
if str(ereq.get("region","")) != "us":
  print("executor requirements mismatch", executor, file=sys.stderr)
  raise SystemExit(1)
PY

TAKEOVER_GET_JSON="$(
  curl -fsS -k --noproxy "*" "${CURL_BASE_OPTS[@]}" \
    -H "Authorization: Bearer ${OIDC_JWT}" \
    "${BROKER_BASE}/v1/teams/${TEAM_ID}/orchestrator/runs/${TAKEOVER_ID}"
)"
python3 - <<PY
import json, sys
obj = json.loads(r'''${TAKEOVER_GET_JSON}''')
run = obj.get("run") or {}
meta = run.get("meta") or {}
owner = str(meta.get("orchestrator_owner",""))
prev = str(meta.get("orchestrator_owner_prev",""))
if owner != "orch_takeover":
  print("expected takeover owner orch_takeover", obj, file=sys.stderr)
  raise SystemExit(1)
if prev != "orch_prev":
  print("expected previous owner orch_prev", obj, file=sys.stderr)
  raise SystemExit(1)
PY

echo "broker_orchestrator_loop_compose_smoke OK"
