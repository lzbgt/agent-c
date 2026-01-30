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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "agentd_config_smoke" \
  --tools none \
  --auth-token "${TOKEN}"

# Wait for health endpoint (health is intentionally auth-free so clients can probe).
agentd_smoke_wait_health "${DAEMON_URL}"

# /api/v1/config without auth should be 401.
code="$(curl -sS --noproxy "*" --max-time 2 -o /dev/null -w '%{http_code}' "${DAEMON_URL}/api/v1/config")"
if [[ "${code}" != "401" ]]; then
  echo "expected 401 for /api/v1/config without auth, got ${code}" >&2
  exit 1
fi

# With auth should succeed and not include secrets.
resp="$(curl -fsS --noproxy "*" --max-time 2 -H "Authorization: Bearer ${TOKEN}" "${DAEMON_URL}/api/v1/config")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp}''')
if not obj.get("ok"):
  print(obj, file=sys.stderr)
  raise SystemExit(1)
daemon = obj.get("daemon") or {}
if daemon.get("listen_host") != "${HOST}":
  print("unexpected listen_host:", daemon.get("listen_host"), file=sys.stderr)
  raise SystemExit(1)
if int(daemon.get("listen_port", -1)) != int("${PORT}"):
  print("unexpected listen_port:", daemon.get("listen_port"), file=sys.stderr)
  raise SystemExit(1)
if daemon.get("auth_enabled") is not True:
  print("expected auth_enabled true", file=sys.stderr)
  raise SystemExit(1)

# Ensure we didn't accidentally leak secrets.
raw = r'''${resp}'''
if "auth_token" in raw:
  print("config response should not include auth_token", file=sys.stderr)
  raise SystemExit(1)
if "\"api_key\"" in raw:
  print("config response should not include provider api_key value", file=sys.stderr)
  raise SystemExit(1)
PY
