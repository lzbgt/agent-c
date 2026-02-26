#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT}/tools/lib/docker_preflight.sh" 2>/dev/null || true
# shellcheck source=tools/lib/python_helpers.sh
source "${ROOT}/tools/lib/python_helpers.sh" 2>/dev/null || true

usage() {
  cat <<'USAGE'
Usage: tools/devstack_agent_down.sh [--state <path>] [--wipe-volumes]

Stops a running devstack started by tools/devstack_agent.sh.
By default, container volumes are preserved so Keycloak tokens remain valid.
USAGE
}

STATE="${ROOT}/out/devstack_state.json"
WIPE_VOLUMES=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --state)
      STATE="$2"
      shift 2
      ;;
    --wipe-volumes)
      WIPE_VOLUMES=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown arg: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ ! -f "${STATE}" ]]; then
  echo "state file not found: ${STATE}" >&2
  exit 1
fi

read_field() {
  local key="$1"
  python3 - <<PY
import json
import sys
with open("${STATE}", "r", encoding="utf-8") as f:
    obj = json.load(f)
val = obj.get("${key}")
if val is None:
    print("")
else:
    print(val)
PY
}

read_agents_pids() {
  local key="$1"
  python3 - <<PY
import json
with open("${STATE}", "r", encoding="utf-8") as f:
    obj = json.load(f)
agents = obj.get("agents") or []
vals = []
for a in agents:
    if isinstance(a, dict):
        v = a.get("${key}")
        if v is not None:
            vals.append(str(v))
print(" ".join(vals))
PY
}

kill_pid() {
  local pid="$1"
  if [[ -n "${pid}" && "${pid}" != "0" ]]; then
    if kill -0 "${pid}" >/dev/null 2>&1; then
      kill "${pid}" >/dev/null 2>&1 || true
      local waited=0
      while kill -0 "${pid}" >/dev/null 2>&1; do
        if (( waited >= 10 )); then
          break
        fi
        sleep 1
        waited=$((waited + 1))
      done
    fi
  fi
}

kill_port_listeners() {
  local port="$1"
  if ! command -v lsof >/dev/null 2>&1; then
    return 0
  fi
  local pids
  pids="$(lsof -t -nP -iTCP:"${port}" -sTCP:LISTEN 2>/dev/null || true)"
  if [[ -z "${pids}" ]]; then
    return 0
  fi
  for pid in ${pids}; do
    kill_pid "${pid}"
  done
}

AGENTD_PID="$(read_field agentd_pid)"
BROKER_PID="$(read_field broker_pid)"
CONNECTOR_PID="$(read_field connector_pid)"
WEBUI_PID="$(read_field webui_pid)"
WEBUI_PORT="$(read_field webui_port)"
AGENTD_PIDS="$(read_agents_pids agentd_pid)"
CONNECTOR_PIDS="$(read_agents_pids connector_pid)"
COMPOSE_PROJECT="$(read_field compose_project)"
COMPOSE_FILE="$(read_field compose_file)"
POSTGRES_PORT="$(read_field postgres_port)"
KEYCLOAK_PORT="$(read_field keycloak_port)"

kill_pid "${WEBUI_PID}"
if [[ -n "${WEBUI_PID}" ]]; then
  kill_port_listeners "${WEBUI_PORT}"
fi
if [[ -n "${CONNECTOR_PIDS}" ]]; then
  for pid in ${CONNECTOR_PIDS}; do
    kill_pid "${pid}"
  done
else
  kill_pid "${CONNECTOR_PID}"
fi
kill_pid "${BROKER_PID}"
if [[ -n "${AGENTD_PIDS}" ]]; then
  for pid in ${AGENTD_PIDS}; do
    kill_pid "${pid}"
  done
else
  kill_pid "${AGENTD_PID}"
fi

if [[ -n "${COMPOSE_FILE}" && -n "${COMPOSE_PROJECT}" ]]; then
  if command -v docker_compose_preflight >/dev/null 2>&1 && docker_compose_preflight "devstack-down" >/dev/null 2>&1; then
    if [[ "${WIPE_VOLUMES}" -eq 1 ]]; then
      POSTGRES_PUBLISHED_PORT="${POSTGRES_PORT}" \
      KEYCLOAK_PUBLISHED_PORT="${KEYCLOAK_PORT}" \
      COMPOSE_PROJECT_NAME="${COMPOSE_PROJECT}" \
      docker compose -f "${COMPOSE_FILE}" down -v --remove-orphans >/dev/null 2>&1 || true
    else
      POSTGRES_PUBLISHED_PORT="${POSTGRES_PORT}" \
      KEYCLOAK_PUBLISHED_PORT="${KEYCLOAK_PORT}" \
      COMPOSE_PROJECT_NAME="${COMPOSE_PROJECT}" \
      docker compose -f "${COMPOSE_FILE}" down --remove-orphans >/dev/null 2>&1 || true
    fi
  else
    echo "devstack stopped (docker not available for compose teardown)" >&2
  fi
fi

echo "devstack stopped"
