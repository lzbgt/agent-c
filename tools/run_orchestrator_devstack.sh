#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STATE_PATH="${ROOT}/out/devstack_state.json"
BROKER_BASE=""
KEYCLOAK_BASE=""
ORCH_ARGS=()
INSECURE="0"
ONCE="0"
LOG_DIR="${ROOT}/out"

usage() {
  cat <<'USAGE'
Usage: tools/run_orchestrator_devstack.sh [options]

Options:
  --state <path>          devstack_state.json path (default: out/devstack_state.json)
  --broker-base <url>     override broker base URL (default: from state)
  --keycloak <url>        override keycloak base URL (default: from state)
  --orchestrator-id <id>  orchestrator id (default: random)
  --status <status>       run status filter (default: running)
  --limit <n>             max runs per team (default: 50)
  --poll-interval <s>     poll interval seconds (default: 5)
  --no-include-planned    skip planned runs
  --once                  run one poll cycle and exit
  --insecure              skip TLS verification (self-signed stacks)
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --state) STATE_PATH="$2"; shift 2 ;;
    --broker-base) BROKER_BASE="$2"; shift 2 ;;
    --keycloak) KEYCLOAK_BASE="$2"; shift 2 ;;
    --orchestrator-id) ORCH_ARGS+=(--orchestrator-id "$2"); shift 2 ;;
    --status) ORCH_ARGS+=(--status "$2"); shift 2 ;;
    --limit) ORCH_ARGS+=(--limit "$2"); shift 2 ;;
    --poll-interval) ORCH_ARGS+=(--poll-interval "$2"s); shift 2 ;;
    --no-include-planned) ORCH_ARGS+=(--include-planned=false); shift ;;
    --once) ONCE="1"; shift ;;
    --insecure) INSECURE="1"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown arg: $1" >&2; usage; exit 1 ;;
  esac
done

if [[ -f "${STATE_PATH}" ]]; then
  if [[ -z "${BROKER_BASE}" ]]; then
    BROKER_BASE="$(python3 - <<PY
import json
with open("${STATE_PATH}") as f:
  st=json.load(f)
print(st.get("broker_base",""))
PY
)"
  fi
  if [[ -z "${KEYCLOAK_BASE}" ]]; then
    KEYCLOAK_BASE="$(python3 - <<PY
import json
with open("${STATE_PATH}") as f:
  st=json.load(f)
print(st.get("keycloak_base",""))
PY
)"
  fi
fi

if [[ -z "${BROKER_BASE}" ]]; then
  echo "missing broker base (set --broker-base or provide devstack_state.json)" >&2
  exit 2
fi
if [[ -z "${KEYCLOAK_BASE}" ]]; then
  echo "missing keycloak base (set --keycloak or provide devstack_state.json)" >&2
  exit 2
fi

OIDC_TOKEN="${BROKER_OIDC_TOKEN:-}"
if [[ -z "${OIDC_TOKEN}" ]]; then
  OIDC_TOKEN="$("${ROOT}/tools/devstack_oidc_token.sh" --state "${STATE_PATH}" --keycloak "${KEYCLOAK_BASE}")"
fi
if [[ -z "${OIDC_TOKEN}" ]]; then
  echo "failed to obtain OIDC token" >&2
  exit 3
fi

mkdir -p "${LOG_DIR}"
TS="$(date +\"%Y%m%d_%H%M%S\")"
LOG_FILE="${LOG_DIR}/orchestrator_${TS}.log"

export BROKER_BASE="${BROKER_BASE}"
export BROKER_OIDC_TOKEN="${OIDC_TOKEN}"

if [[ "${INSECURE}" == "1" ]]; then
  ORCH_ARGS+=(--insecure)
fi
if [[ "${ONCE}" == "1" ]]; then
  ORCH_ARGS+=(--once)
fi

(cd "${ROOT}/broker" && go run ./cmd/agentd-orchestrator "${ORCH_ARGS[@]}") >"${LOG_FILE}" 2>&1
echo "[orchestrator] done (log: ${LOG_FILE})"
