#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

STATE_PATH="${STATE_PATH:-${ROOT}/out/devstack_state.json}"
KEYCLOAK_BASE="${KEYCLOAK_BASE:-}"
REALM="${OIDC_REALM:-agentd}"
CLIENT_ID="${OIDC_CLIENT_ID:-agentd-broker-dev}"
USERNAME="${OIDC_USERNAME:-test}"
PASSWORD="${OIDC_PASSWORD:-test}"

usage() {
  cat <<'USAGE'
Usage: tools/devstack_oidc_token.sh [options]

Options:
  --state <path>        devstack_state.json path (default: out/devstack_state.json)
  --keycloak <url>      override keycloak base URL (default: from state)
  --realm <name>        realm name (default: agentd)
  --client-id <id>      client id (default: agentd-broker-dev)
  --user <username>     username (default: test)
  --password <password> password (default: test)
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --state) STATE_PATH="$2"; shift 2 ;;
    --keycloak) KEYCLOAK_BASE="$2"; shift 2 ;;
    --realm) REALM="$2"; shift 2 ;;
    --client-id) CLIENT_ID="$2"; shift 2 ;;
    --user) USERNAME="$2"; shift 2 ;;
    --password) PASSWORD="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown arg: $1" >&2; usage; exit 1 ;;
  esac
 done

if [[ -z "${KEYCLOAK_BASE}" ]]; then
  if [[ -f "${STATE_PATH}" ]]; then
    KEYCLOAK_BASE="$(python3 - <<PY
import json
with open("${STATE_PATH}") as f:
    st=json.load(f)
print(st.get("keycloak_base",""))
PY
)"
  fi
fi

if [[ "${KEYCLOAK_BASE}" =~ ^http://keycloak\.lvh\.me:([0-9]+)$ ]]; then
  if ! curl -fsS --max-time 2 "${KEYCLOAK_BASE}/realms/${REALM}/.well-known/openid-configuration" >/dev/null 2>&1; then
    KEYCLOAK_BASE="http://127.0.0.1:${BASH_REMATCH[1]}"
  fi
fi

if [[ -z "${KEYCLOAK_BASE}" ]]; then
  echo "missing keycloak base (set --keycloak or provide devstack_state.json)" >&2
  exit 2
fi

if [[ -z "${USERNAME}" || -z "${PASSWORD}" || -z "${CLIENT_ID}" ]]; then
  echo "missing username/password/client-id" >&2
  exit 2
fi

TOKEN_JSON="$(env -u HTTPS_PROXY -u https_proxy -u HTTP_PROXY -u http_proxy -u ALL_PROXY -u all_proxy \
  curl -fsS -k \
    -d "grant_type=password" \
    -d "client_id=${CLIENT_ID}" \
    -d "username=${USERNAME}" \
    -d "password=${PASSWORD}" \
    "${KEYCLOAK_BASE}/realms/${REALM}/protocol/openid-connect/token")"

python3 - <<PY
import json,sys
obj=json.loads(r'''${TOKEN_JSON}''')
print(obj.get("access_token",""))
PY
