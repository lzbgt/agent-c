#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT}/tools/lib/docker_preflight.sh" 2>/dev/null || true

usage() {
  cat <<'USAGE'
Usage: tools/devstack_agent_down.sh [--state <path>]

Stops a running devstack started by tools/devstack_agent.sh.
USAGE
}

STATE="${ROOT}/out/devstack_state.json"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --state)
      STATE="$2"
      shift 2
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

kill_pid() {
  local pid="$1"
  if [[ -n "${pid}" && "${pid}" != "0" ]]; then
    if kill -0 "${pid}" >/dev/null 2>&1; then
      kill "${pid}" >/dev/null 2>&1 || true
    fi
  fi
}

AGENTD_PID="$(read_field agentd_pid)"
BROKER_PID="$(read_field broker_pid)"
CONNECTOR_PID="$(read_field connector_pid)"
WEBUI_PID="$(read_field webui_pid)"
COMPOSE_PROJECT="$(read_field compose_project)"
COMPOSE_FILE="$(read_field compose_file)"
POSTGRES_PORT="$(read_field postgres_port)"
KEYCLOAK_PORT="$(read_field keycloak_port)"

kill_pid "${WEBUI_PID}"
kill_pid "${CONNECTOR_PID}"
kill_pid "${BROKER_PID}"
kill_pid "${AGENTD_PID}"

if [[ -n "${COMPOSE_FILE}" && -n "${COMPOSE_PROJECT}" ]]; then
  if docker_preflight "devstack-down" >/dev/null 2>&1; then
    POSTGRES_PUBLISHED_PORT="${POSTGRES_PORT}" \
    KEYCLOAK_PUBLISHED_PORT="${KEYCLOAK_PORT}" \
    COMPOSE_PROJECT_NAME="${COMPOSE_PROJECT}" \
    docker compose -f "${COMPOSE_FILE}" down -v --remove-orphans >/dev/null 2>&1 || true
  else
    echo "devstack stopped (docker not available for compose teardown)" >&2
  fi
fi

echo "devstack stopped"
