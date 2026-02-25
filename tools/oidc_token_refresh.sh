#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STATE_PATH="${ROOT}/out/devstack_state.json"
KEYCLOAK_BASE=""
TOKEN_URL=""
ISSUER=""
REALM="${OIDC_REALM:-agentd}"
CLIENT_ID="${OIDC_CLIENT_ID:-}"
CLIENT_SECRET="${OIDC_CLIENT_SECRET:-}"
USERNAME="${OIDC_USERNAME:-}"
PASSWORD="${OIDC_PASSWORD:-}"
SCOPE="${OIDC_SCOPE:-}"
REFRESH_TOKEN="${OIDC_REFRESH_TOKEN:-}"
OUTPUT="${OIDC_TOKEN_FILE:-}"
REFRESH_BEFORE=60
MIN_INTERVAL=20
ONCE="0"
INSECURE="0"
PRINT_TOKEN="0"

usage() {
  cat <<'USAGE'
Usage: tools/oidc_token_refresh.sh [options]

Options:
  --state <path>         devstack_state.json path (default: out/devstack_state.json)
  --keycloak <url>       keycloak base URL (builds token URL)
  --realm <name>         realm name (default: agentd)
  --issuer <url>         OIDC issuer URL (resolve token endpoint)
  --token-url <url>      explicit token endpoint URL
  --client-id <id>       OIDC client id (required)
  --client-secret <sec>  OIDC client secret (optional)
  --user <username>      username (password grant)
  --password <password>  password (password grant)
  --refresh-token <tok>  refresh token (refresh_token grant)
  --scope <scope>        optional scope value
  --output <path>        write access token to file
  --refresh-before <s>   refresh this many seconds before expiry (default: 60)
  --min-interval <s>     minimum sleep between refreshes (default: 20)
  --once                fetch once and exit
  --insecure            skip TLS verification
  --print               print access token to stdout (best-effort)
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --state) STATE_PATH="$2"; shift 2 ;;
    --keycloak) KEYCLOAK_BASE="$2"; shift 2 ;;
    --realm) REALM="$2"; shift 2 ;;
    --issuer) ISSUER="$2"; shift 2 ;;
    --token-url) TOKEN_URL="$2"; shift 2 ;;
    --client-id) CLIENT_ID="$2"; shift 2 ;;
    --client-secret) CLIENT_SECRET="$2"; shift 2 ;;
    --user) USERNAME="$2"; shift 2 ;;
    --password) PASSWORD="$2"; shift 2 ;;
    --refresh-token) REFRESH_TOKEN="$2"; shift 2 ;;
    --scope) SCOPE="$2"; shift 2 ;;
    --output) OUTPUT="$2"; shift 2 ;;
    --refresh-before) REFRESH_BEFORE="$2"; shift 2 ;;
    --min-interval) MIN_INTERVAL="$2"; shift 2 ;;
    --once) ONCE="1"; shift ;;
    --insecure) INSECURE="1"; shift ;;
    --print) PRINT_TOKEN="1"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown arg: $1" >&2; usage; exit 1 ;;
  esac
done

if [[ -z "${TOKEN_URL}" && -z "${KEYCLOAK_BASE}" && -z "${ISSUER}" && -f "${STATE_PATH}" ]]; then
  KEYCLOAK_BASE="$(python3 - <<PY
import json
with open("${STATE_PATH}") as f:
  st=json.load(f)
print(st.get("keycloak_base",""))
PY
)"
fi

if [[ -z "${TOKEN_URL}" && -n "${ISSUER}" ]]; then
  TOKEN_URL="$(env -u HTTPS_PROXY -u https_proxy -u HTTP_PROXY -u http_proxy -u ALL_PROXY -u all_proxy \
    curl -fsS ${INSECURE:+-k} "${ISSUER%/}/.well-known/openid-configuration" | \
    python3 - <<'PY'
import json,sys
obj=json.load(sys.stdin)
print(obj.get('token_endpoint',''))
PY
  )"
fi

if [[ -z "${TOKEN_URL}" && -n "${KEYCLOAK_BASE}" ]]; then
  TOKEN_URL="${KEYCLOAK_BASE%/}/realms/${REALM}/protocol/openid-connect/token"
fi

if [[ -z "${TOKEN_URL}" ]]; then
  echo "missing token endpoint (set --token-url, --issuer, or --keycloak)" >&2
  exit 2
fi

if [[ -z "${CLIENT_ID}" ]]; then
  echo "missing client id (set --client-id)" >&2
  exit 2
fi

if [[ -n "${OUTPUT}" ]]; then
  mkdir -p "$(dirname "${OUTPUT}")"
fi

fetch_token() {
  local grant=""
  local -a data
  local -a args
  if [[ -n "${REFRESH_TOKEN}" ]]; then
    grant="refresh_token"
    data+=("grant_type=${grant}")
    data+=("refresh_token=${REFRESH_TOKEN}")
  elif [[ -n "${USERNAME}" && -n "${PASSWORD}" ]]; then
    grant="password"
    data+=("grant_type=${grant}")
    data+=("username=${USERNAME}")
    data+=("password=${PASSWORD}")
  elif [[ -n "${CLIENT_SECRET}" ]]; then
    grant="client_credentials"
    data+=("grant_type=${grant}")
  else
    echo "missing grant parameters (set refresh token, user/password, or client secret)" >&2
    return 1
  fi
  data+=("client_id=${CLIENT_ID}")
  if [[ -n "${CLIENT_SECRET}" ]]; then
    data+=("client_secret=${CLIENT_SECRET}")
  fi
  if [[ -n "${SCOPE}" ]]; then
    data+=("scope=${SCOPE}")
  fi

  for item in "${data[@]}"; do
    args+=("-d" "${item}")
  done

  env -u HTTPS_PROXY -u https_proxy -u HTTP_PROXY -u http_proxy -u ALL_PROXY -u all_proxy \
    curl -fsS ${INSECURE:+-k} \
      "${args[@]}" \
      "${TOKEN_URL}"
}

while true; do
  TOKEN_JSON="$(fetch_token)" || exit 1
  IFS=$'\n' read -r ACCESS_TOKEN EXPIRES_IN NEW_REFRESH_TOKEN <<__TOKEN_EOF__
$(python3 - <<'PY' <<<"${TOKEN_JSON}"
import json,sys
obj=json.load(sys.stdin)
print(obj.get('access_token',''))
print(obj.get('expires_in',0) or 0)
print(obj.get('refresh_token',''))
PY
)
__TOKEN_EOF__
  if [[ -z "${ACCESS_TOKEN}" ]]; then
    echo "failed to parse access token" >&2
    exit 2
  fi

  if [[ -n "${NEW_REFRESH_TOKEN}" ]]; then
    REFRESH_TOKEN="${NEW_REFRESH_TOKEN}"
  fi

  if [[ -n "${OUTPUT}" ]]; then
    umask 077
    printf '%s' "${ACCESS_TOKEN}" > "${OUTPUT}"
  fi
  if [[ "${PRINT_TOKEN}" == "1" ]]; then
    printf '%s\n' "${ACCESS_TOKEN}"
  fi

  if [[ "${ONCE}" == "1" ]]; then
    exit 0
  fi

  sleep_for=300
  if [[ "${EXPIRES_IN}" =~ ^[0-9]+$ && "${EXPIRES_IN}" -gt 0 ]]; then
    sleep_for=$((EXPIRES_IN - REFRESH_BEFORE))
  fi
  if [[ "${sleep_for}" -lt "${MIN_INTERVAL}" ]]; then
    sleep_for="${MIN_INTERVAL}"
  fi
  sleep "${sleep_for}"
done
