#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STATE_PATH="${ROOT}/out/devstack_state.json"
BROKER_BASE=""
KEYCLOAK_BASE=""
SPAWN_COMMAND=""
SPAWN_ALLOCATOR="${SPAWN_ALLOCATOR:-}"
INSECURE="0"
ONCE="0"
ORCH_ARGS=()
SPAWN_ARGS=()

usage() {
  cat <<'USAGE'
Usage: tools/run_autonomous_devstack.sh [options]

Options:
  --state <path>        devstack_state.json path (default: out/devstack_state.json)
  --broker-base <url>   override broker base URL (default: from state)
  --keycloak <url>      override keycloak base URL (default: from state)
  --orchestrator-id <id> orchestrator id (default: random)
  --command <cmd>       spawn command (optional when --allocator is set)
  --allocator           use broker runtime member allocator (no command required)
  --status <status>     spawn request status filter (default: requested)
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
    --orchestrator-id) ORCH_ARGS+=(--orchestrator-id "$2"); shift 2 ;;
    --command) SPAWN_COMMAND="$2"; shift 2 ;;
    --allocator) SPAWN_ALLOCATOR="1"; shift ;;
    --status) SPAWN_ARGS+=(--status "$2"); shift 2 ;;
    --limit) SPAWN_ARGS+=(--limit "$2"); shift 2 ;;
    --poll-interval) SPAWN_ARGS+=(--poll-interval "$2"s); shift 2 ;;
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
if [[ -z "${SPAWN_COMMAND}" && "${SPAWN_ALLOCATOR}" != "1" ]]; then
  SPAWN_ALLOCATOR="1"
fi

OIDC_TOKEN="${BROKER_OIDC_TOKEN:-}"
if [[ -z "${OIDC_TOKEN}" ]]; then
  OIDC_TOKEN="$("${ROOT}/tools/devstack_oidc_token.sh" --state "${STATE_PATH}" --keycloak "${KEYCLOAK_BASE}")"
fi
if [[ -z "${OIDC_TOKEN}" ]]; then
  echo "failed to obtain OIDC token" >&2
  exit 3
fi

export BROKER_BASE="${BROKER_BASE}"
export BROKER_OIDC_TOKEN="${OIDC_TOKEN}"
export SPAWN_COMMAND="${SPAWN_COMMAND}"
export SPAWN_ALLOCATOR="${SPAWN_ALLOCATOR}"

if [[ "${INSECURE}" == "1" ]]; then
  ORCH_ARGS+=(--insecure)
  SPAWN_ARGS+=(--insecure)
fi
if [[ "${ONCE}" == "1" ]]; then
  ORCH_ARGS+=(--once)
  SPAWN_ARGS+=(--once)
fi
if [[ "${SPAWN_ALLOCATOR}" == "1" ]]; then
  SPAWN_ARGS+=(--allocator)
fi

pids=()
cleanup() {
  for pid in "${pids[@]}"; do
    kill "${pid}" >/dev/null 2>&1 || true
  done
  wait >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

orch_cmd=("${ROOT}/tools/run_orchestrator_devstack.sh" --broker-base "${BROKER_BASE}")
orch_cmd+=("${ORCH_ARGS[@]}")
"${orch_cmd[@]}" &
pids+=("$!")

spawn_cmd=("${ROOT}/tools/run_spawn_adapter_devstack.sh" --broker-base "${BROKER_BASE}" --keycloak "${KEYCLOAK_BASE}")
spawn_cmd+=("${SPAWN_ARGS[@]}")
if [[ -n "${SPAWN_COMMAND}" ]]; then
  spawn_cmd+=(--command "${SPAWN_COMMAND}")
fi
"${spawn_cmd[@]}" &
pids+=("$!")

wait "${pids[@]}"
