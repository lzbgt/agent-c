#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="compose"
STATE_PATH="${ROOT}/out/devstack_state.json"
BROKER_BASE=""
KEYCLOAK_BASE=""
TOKEN_FILE="${BROKER_OIDC_TOKEN_FILE:-}"
INSECURE="0"
ONCE="0"
COMPOSE_BUILD="1"
COMPOSE_PULL="0"
COMPOSE_CLEAN="1"

usage() {
  cat <<'USAGE'
Usage: tools/automouse_pack.sh [options]

Modes:
  --compose           Run the Docker Compose automouse stack (default).
  --devstack          Run the local autonomous devstack (orchestrator + spawn adapter).

Common options:
  --broker-base <url> Broker base URL (devstack mode).
  --keycloak <url>    Keycloak base URL (devstack mode).
  --state <path>      devstack_state.json path (devstack mode).
  --token-file <path> Broker OIDC token file (devstack mode).
  --insecure          Skip TLS verification (devstack mode).
  --once              Run a single poll cycle (devstack mode).

Compose options:
  --no-build          Skip docker image builds.
  --pull              Pull missing images when build is disabled.
  --no-clean          Skip docker compose down -v before up.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --compose) MODE="compose"; shift ;;
    --devstack) MODE="devstack"; shift ;;
    --broker-base) BROKER_BASE="$2"; shift 2 ;;
    --keycloak) KEYCLOAK_BASE="$2"; shift 2 ;;
    --state) STATE_PATH="$2"; shift 2 ;;
    --token-file) TOKEN_FILE="$2"; shift 2 ;;
    --insecure) INSECURE="1"; shift ;;
    --once) ONCE="1"; shift ;;
    --no-build) COMPOSE_BUILD="0"; shift ;;
    --pull) COMPOSE_PULL="1"; shift ;;
    --no-clean) COMPOSE_CLEAN="0"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown arg: $1" >&2; usage; exit 1 ;;
  esac
done

if [[ "${MODE}" == "compose" ]]; then
  export COMPOSE_AUTONOMOUS=1
  export COMPOSE_AUTOMOUSE=1
  export COMPOSE_BUILD="${COMPOSE_BUILD}"
  export COMPOSE_PULL="${COMPOSE_PULL}"
  export COMPOSE_CLEAN="${COMPOSE_CLEAN}"
  "${ROOT}/tools/verify_compose_stack.sh"
  exit 0
fi

if [[ "${MODE}" != "devstack" ]]; then
  echo "invalid mode: ${MODE}" >&2
  exit 1
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

if [[ -n "${BROKER_BASE}" ]]; then
  curl_flags=("-fsS")
  if [[ "${INSECURE}" == "1" ]]; then
    curl_flags+=("-k")
  fi
  if ! curl "${curl_flags[@]}" "${BROKER_BASE}/healthz" >/dev/null 2>&1; then
    echo "broker healthz check failed: ${BROKER_BASE}/healthz" >&2
    exit 2
  fi
fi

cmd=("${ROOT}/tools/run_autonomous_devstack.sh")
cmd+=(--state "${STATE_PATH}")
if [[ -n "${BROKER_BASE}" ]]; then
  cmd+=(--broker-base "${BROKER_BASE}")
fi
if [[ -n "${KEYCLOAK_BASE}" ]]; then
  cmd+=(--keycloak "${KEYCLOAK_BASE}")
fi
if [[ -n "${TOKEN_FILE}" ]]; then
  cmd+=(--token-file "${TOKEN_FILE}")
fi
if [[ "${INSECURE}" == "1" ]]; then
  cmd+=(--insecure)
fi
if [[ "${ONCE}" == "1" ]]; then
  cmd+=(--once)
fi
exec "${cmd[@]}"
