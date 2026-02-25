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
if ! docker_compose_preflight "broker-oidc-refresh"; then
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

STACK_STARTED="0"

cleanup() {
  if [[ "${STACK_STARTED}" == "1" && -n "${COMPOSE_PROJECT_NAME:-}" ]]; then
    (cd "${ROOT}" && docker compose -p "${COMPOSE_PROJECT_NAME}" down -v --remove-orphans >/dev/null 2>&1) || true
  fi
}
trap cleanup EXIT

start_compose_stack() {
  BROKER_PUBLISHED_PORT="$(agentd_smoke_pick_port)"
  KEYCLOAK_PUBLISHED_PORT="$(agentd_smoke_pick_port)"
  POSTGRES_PUBLISHED_PORT="$(agentd_smoke_pick_port)"
  AGENTD_PUBLISHED_PORT="$(agentd_smoke_pick_port)"
  WEBUI_PUBLISHED_PORT="$(agentd_smoke_pick_port)"
  export BROKER_PUBLISHED_PORT KEYCLOAK_PUBLISHED_PORT POSTGRES_PUBLISHED_PORT AGENTD_PUBLISHED_PORT WEBUI_PUBLISHED_PORT

  VERIFY_LOG="${LOG_DIR}/broker_oidc_refresh_compose_verify.log"
  if ! "${ROOT}/tools/verify_compose_stack.sh" >"${VERIFY_LOG}" 2>&1; then
    cat "${VERIFY_LOG}" >&2 || true
    if [[ -n "${COMPOSE_PROJECT_NAME:-}" ]]; then
      (cd "${ROOT}" && docker compose -p "${COMPOSE_PROJECT_NAME}" down -v --remove-orphans >/dev/null 2>&1) || true
    fi
    exit 1
  fi

  AUTON_LOG="${LOG_DIR}/broker_oidc_refresh_compose_autonomous.log"
  if ! (cd "${ROOT}" && docker compose -p "${COMPOSE_PROJECT_NAME}" -f docker-compose.yml -f docker/compose.autonomous.yml up -d) >"${AUTON_LOG}" 2>&1; then
    cat "${AUTON_LOG}" >&2 || true
    if [[ -n "${COMPOSE_PROJECT_NAME:-}" ]]; then
      (cd "${ROOT}" && docker compose -p "${COMPOSE_PROJECT_NAME}" down -v --remove-orphans >/dev/null 2>&1) || true
    fi
    exit 1
  fi
  STACK_STARTED="1"
}

if [[ -n "${BROKER_PUBLISHED_PORT}" && -n "${KEYCLOAK_PUBLISHED_PORT}" && -n "${AGENTD_PUBLISHED_PORT}" && -n "${WEBUI_PUBLISHED_PORT}" ]]; then
  start_compose_stack
else
  start_compose_stack
fi

BROKER_BASE="https://127.0.0.1:${BROKER_PUBLISHED_PORT}"

TOKEN_FILE="/run/agentd/broker_oidc_token.txt"

deadline=$(( $(date +%s) + 120 ))
while true; do
  now=$(date +%s)
  if [[ "${now}" -gt "${deadline}" ]]; then
    echo "timed out waiting for token file" >&2
    exit 1
  fi
  if docker compose -p "${COMPOSE_PROJECT_NAME}" exec -T oidc-refresh sh -c "test -s ${TOKEN_FILE}" >/dev/null 2>&1; then
    break
  fi
  sleep 1
 done

OIDC_JWT="$(docker compose -p "${COMPOSE_PROJECT_NAME}" exec -T oidc-refresh sh -c "cat ${TOKEN_FILE}" | tr -d '\r\n')"
if [[ -z "${OIDC_JWT}" ]]; then
  echo "token file empty" >&2
  exit 1
fi

TEAMS_JSON="$(
  curl -fsS -k --noproxy "*" "${CURL_BASE_OPTS[@]}" \
    -H "Authorization: Bearer ${OIDC_JWT}" \
    "${BROKER_BASE}/v1/teams"
)"
python3 - <<PY
import json,sys
obj = json.loads(r'''${TEAMS_JSON}''')
if not obj.get('ok'):
  print('broker teams list failed', obj, file=sys.stderr)
  raise SystemExit(1)
PY

# Ensure orchestrator/spawn adapter can use the token file even if env token is bad.
docker compose -p "${COMPOSE_PROJECT_NAME}" exec -T orchestrator sh -c \
  "BROKER_OIDC_TOKEN=bad BROKER_OIDC_TOKEN_FILE=${TOKEN_FILE} /usr/local/bin/agentd-orchestrator --once --status planned" >/dev/null

docker compose -p "${COMPOSE_PROJECT_NAME}" exec -T spawn-adapter sh -c \
  "BROKER_OIDC_TOKEN=bad BROKER_OIDC_TOKEN_FILE=${TOKEN_FILE} SPAWN_ALLOCATOR=1 /usr/local/bin/agentd-spawn-adapter --once --allocator" >/dev/null

echo "broker_oidc_refresh_compose_smoke OK"
