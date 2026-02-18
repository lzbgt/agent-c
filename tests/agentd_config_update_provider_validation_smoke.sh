#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "missing agentd binary path arg" >&2
  exit 2
fi

HOST="127.0.0.1"
PORT="$(agentd_smoke_pick_port)"
TOKEN="test_token_123"

trap agentd_smoke_stop EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "agentd_config_update_provider_validation_smoke" \
  --tools none \
  --auth-token "${TOKEN}"

agentd_smoke_wait_health "${DAEMON_URL}"

check_invalid() {
  local payload="$1"
  local code
  local body
  body="$(mktemp)"
  code="$(curl -sS --noproxy "*" --max-time 5 -o "${body}" -w '%{http_code}' \
    -H "Authorization: Bearer ${TOKEN}" \
    -H "Content-Type: application/json" \
    -d "${payload}" \
    "${DAEMON_URL}/api/v1/config/update")"
  if [[ "${code}" != "400" ]]; then
    echo "expected 400 for payload ${payload}, got ${code}" >&2
    cat "${body}" >&2 || true
    rm -f "${body}"
    exit 1
  fi
  python3 - <<PY
import json, sys
obj = json.loads(open("${body}").read())
if obj.get("ok") is not False:
  print("expected ok=false", obj, file=sys.stderr)
  raise SystemExit(1)
if "invalid provider" not in (obj.get("error") or ""):
  print("expected invalid provider error", obj, file=sys.stderr)
  raise SystemExit(1)
PY
  rm -f "${body}"
}

check_invalid '{"provider":"unknown","api_key":"sk-test"}'
check_invalid '{"provider_keys":{"unknown":"sk-test"}}'
