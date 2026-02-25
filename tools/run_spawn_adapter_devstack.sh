#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STATE_PATH="${ROOT}/out/devstack_state.json"
BROKER_BASE=""
KEYCLOAK_BASE=""
TOKEN_FILE="${BROKER_OIDC_TOKEN_FILE:-}"
SPAWN_COMMAND=""
SPAWN_ALLOCATOR="${SPAWN_ALLOCATOR:-}"
ADAPTER_ARGS=()
INSECURE="0"
ONCE="0"
LOG_DIR="${ROOT}/out"

usage() {
  cat <<'USAGE'
Usage: tools/run_spawn_adapter_devstack.sh [options]

Options:
  --state <path>        devstack_state.json path (default: out/devstack_state.json)
  --broker-base <url>   override broker base URL (default: from state)
  --keycloak <url>      override keycloak base URL (default: from state)
  --token-file <path>   path to broker OIDC token file (env: BROKER_OIDC_TOKEN_FILE)
  --command <cmd>       spawn command (required if SPAWN_COMMAND env not set)
  --allocator           use broker runtime member allocator (no command required)
  --adapter-id <id>     adapter id (default: random)
  --status <status>     status filter (default: requested)
  --limit <n>           max requests per team (default: 50)
  --poll-interval <s>   poll interval seconds (default: 3)
  --once                run one poll cycle and exit
  --insecure            skip TLS verification (self-signed stacks)
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --state) STATE_PATH="$2"; shift 2 ;;
    --broker-base) BROKER_BASE="$2"; shift 2 ;;
    --keycloak) KEYCLOAK_BASE="$2"; shift 2 ;;
    --token-file) TOKEN_FILE="$2"; shift 2 ;;
    --command) SPAWN_COMMAND="$2"; shift 2 ;;
    --allocator) SPAWN_ALLOCATOR="1"; shift ;;
    --adapter-id) ADAPTER_ARGS+=(--adapter-id "$2"); shift 2 ;;
    --status) ADAPTER_ARGS+=(--status "$2"); shift 2 ;;
    --limit) ADAPTER_ARGS+=(--limit "$2"); shift 2 ;;
    --poll-interval) ADAPTER_ARGS+=(--poll-interval "$2"s); shift 2 ;;
    --once) ONCE="1"; shift ;;
    --insecure) INSECURE="1"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown arg: $1" >&2; usage; exit 1 ;;
  esac
done

if [[ -z "${SPAWN_COMMAND}" ]]; then
  SPAWN_COMMAND="${SPAWN_COMMAND:-}"
fi
if [[ -z "${SPAWN_ALLOCATOR}" ]]; then
  SPAWN_ALLOCATOR="${SPAWN_ALLOCATOR:-}"
fi

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
if [[ -z "${SPAWN_COMMAND}" && "${SPAWN_ALLOCATOR}" != "1" ]]; then
  echo "missing spawn command (set --command/SPAWN_COMMAND) or enable --allocator" >&2
  exit 2
fi

OIDC_TOKEN="${BROKER_OIDC_TOKEN:-}"
if [[ -n "${TOKEN_FILE}" && -z "${OIDC_TOKEN}" && ! -s "${TOKEN_FILE}" ]]; then
  OIDC_TOKEN="$("${ROOT}/tools/devstack_oidc_token.sh" --state "${STATE_PATH}" --keycloak "${KEYCLOAK_BASE}")"
elif [[ -z "${TOKEN_FILE}" && -z "${OIDC_TOKEN}" ]]; then
  OIDC_TOKEN="$("${ROOT}/tools/devstack_oidc_token.sh" --state "${STATE_PATH}" --keycloak "${KEYCLOAK_BASE}")"
fi
if [[ -z "${OIDC_TOKEN}" && -z "${TOKEN_FILE}" ]]; then
  echo "failed to obtain OIDC token" >&2
  exit 3
fi
if [[ -n "${TOKEN_FILE}" && -n "${OIDC_TOKEN}" ]]; then
  mkdir -p "$(dirname "${TOKEN_FILE}")"
  printf '%s' "${OIDC_TOKEN}" > "${TOKEN_FILE}"
fi
if [[ -n "${TOKEN_FILE}" && ! -s "${TOKEN_FILE}" ]]; then
  echo "warning: OIDC token file is empty (${TOKEN_FILE}); spawn adapter requests may fail until refreshed" >&2
fi

mkdir -p "${LOG_DIR}"
TS="$(date +\"%Y%m%d_%H%M%S\")"
LOG_FILE="${LOG_DIR}/spawn_adapter_${TS}.log"

export BROKER_BASE="${BROKER_BASE}"
if [[ -n "${OIDC_TOKEN}" ]]; then
  export BROKER_OIDC_TOKEN="${OIDC_TOKEN}"
fi
if [[ -n "${TOKEN_FILE}" ]]; then
  export BROKER_OIDC_TOKEN_FILE="${TOKEN_FILE}"
fi
export SPAWN_COMMAND="${SPAWN_COMMAND}"
export SPAWN_ALLOCATOR="${SPAWN_ALLOCATOR}"

if [[ "${INSECURE}" == "1" ]]; then
  ADAPTER_ARGS+=(--insecure)
fi
if [[ "${ONCE}" == "1" ]]; then
  ADAPTER_ARGS+=(--once)
fi
if [[ "${SPAWN_ALLOCATOR}" == "1" ]]; then
  ADAPTER_ARGS+=(--allocator)
fi

(cd "${ROOT}/broker" && go run ./cmd/agentd-spawn-adapter "${ADAPTER_ARGS[@]}") >"${LOG_FILE}" 2>&1
echo "[spawn-adapter] done (log: ${LOG_FILE})"
