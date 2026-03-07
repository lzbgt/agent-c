#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${ROOT}/tests/lib/agentd_smoke_lib.sh"
# shellcheck source=tools/lib/docker_preflight.sh
source "${ROOT}/tools/lib/docker_preflight.sh"

if ! docker_compose_preflight "compose-broker-smokes"; then
  exit 77
fi

OUT_DIR="${ROOT}/out"
mkdir -p "${OUT_DIR}"

ts="$(date +%Y%m%d_%H%M%S)"
VERIFY_LOG="${OUT_DIR}/compose_broker_smokes_${ts}_stack.log"

run_logged() {
  local label="${1}"
  shift
  echo "[compose-broker] ${label}"
  if ! "$@"; then
    echo "[compose-broker] FAILED: ${label}" >&2
    return 1
  fi
  echo "[compose-broker] OK: ${label}"
}

if [[ -z "${BROKER_PUBLISHED_PORT:-}" ]]; then
  export BROKER_PUBLISHED_PORT="$(agentd_smoke_pick_port)"
fi
if [[ -z "${KEYCLOAK_PUBLISHED_PORT:-}" ]]; then
  export KEYCLOAK_PUBLISHED_PORT="$(agentd_smoke_pick_port)"
fi
if [[ -z "${POSTGRES_PUBLISHED_PORT:-}" ]]; then
  export POSTGRES_PUBLISHED_PORT="$(agentd_smoke_pick_port)"
fi
if [[ -z "${AGENTD_PUBLISHED_PORT:-}" ]]; then
  export AGENTD_PUBLISHED_PORT="$(agentd_smoke_pick_port)"
fi
if [[ -z "${WEBUI_PUBLISHED_PORT:-}" ]]; then
  export WEBUI_PUBLISHED_PORT="$(agentd_smoke_pick_port)"
fi
export COMPOSE_PROJECT_NAME="${COMPOSE_PROJECT_NAME:-agent_ci_${WEBUI_PUBLISHED_PORT}}"
export AGENT_SMOKE_USE_PUBLISHED_PORTS=1

cleanup() {
  if [[ "${COMPOSE_CLEANUP:-1}" != "1" ]]; then
    return 0
  fi
  if docker_compose_preflight "compose-broker-smokes-cleanup" >/dev/null 2>&1; then
    (cd "${ROOT}" && docker compose -p "${COMPOSE_PROJECT_NAME}" down -v --remove-orphans >/dev/null 2>&1) || true
  fi
}
trap cleanup EXIT

run_logged \
  "stack bootstrap" \
  bash -lc "cd '${ROOT}' && COMPOSE_CLEAN=1 tools/verify_compose_stack.sh > '${VERIFY_LOG}' 2>&1"

smokes=(
  "tests/broker_team_runs_compose_smoke.sh"
  "tests/broker_team_runs_quorum_compose_smoke.sh"
  "tests/broker_team_run_events_sse_compose_smoke.sh"
)

for smoke in "${smokes[@]}"; do
  run_logged "$(basename "${smoke}")" bash "${ROOT}/${smoke}"
done

echo "[compose-broker] complete (project=${COMPOSE_PROJECT_NAME}, log=${VERIFY_LOG})"
