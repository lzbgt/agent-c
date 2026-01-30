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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "agentd_auth_smoke" \
  --tools none \
  --auth-token "${TOKEN}"

# Wait for health endpoint (health is intentionally auth-free so clients can probe).
agentd_smoke_wait_health "${DAEMON_URL}"

# /api/v1/tools without auth should be 401.
code="$(curl -sS --noproxy "*" --max-time 2 -o /dev/null -w '%{http_code}' "${DAEMON_URL}/api/v1/tools")"
if [[ "${code}" != "401" ]]; then
  echo "expected 401 for /api/v1/tools without auth, got ${code}" >&2
  exit 1
fi

# With auth should succeed.
code="$(curl -sS --noproxy "*" --max-time 2 -H "Authorization: Bearer ${TOKEN}" -o /dev/null -w '%{http_code}' "${DAEMON_URL}/api/v1/tools")"
if [[ "${code}" != "200" ]]; then
  echo "expected 200 for /api/v1/tools with auth, got ${code}" >&2
  exit 1
fi

resp="$(curl -sS --noproxy "*" --max-time 2 -H "Authorization: Bearer ${TOKEN}" "${DAEMON_URL}/api/v1/tools")"
python3 - <<PY
import json, sys
obj = json.loads(r'''${resp}''')
if not obj.get("ok"):
  print(obj, file=sys.stderr)
  raise SystemExit(1)
PY
