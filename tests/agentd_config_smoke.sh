#!/usr/bin/env bash
set -euo pipefail

AGENTD_BIN="${1:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "missing agentd binary path arg" >&2
  exit 2
fi

PORT="$(( (RANDOM % 20000) + 20000 ))"
HOST="127.0.0.1"
DAEMON_URL="http://${HOST}:${PORT}"
TOKEN="test_token_123"

cleanup() {
  if [[ -n "${AGENTD_PID:-}" ]]; then
    kill -TERM "${AGENTD_PID}" >/dev/null 2>&1 || true
    for _ in $(seq 1 30); do
      if ! kill -0 "${AGENTD_PID}" >/dev/null 2>&1; then
        break
      fi
      sleep 0.1
    done
    if kill -0 "${AGENTD_PID}" >/dev/null 2>&1; then
      kill -KILL "${AGENTD_PID}" >/dev/null 2>&1 || true
    fi
    wait "${AGENTD_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

LOG_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/build"
mkdir -p "${LOG_DIR}"

"${AGENTD_BIN}" \
  --host "${HOST}" \
  --port "${PORT}" \
  --tools none \
  --auth-token "${TOKEN}" \
  > "${LOG_DIR}/agentd_config_smoke.stdout.log" 2> "${LOG_DIR}/agentd_config_smoke.stderr.log" &
AGENTD_PID=$!

# Wait for health endpoint (health is intentionally auth-free so clients can probe).
for _ in $(seq 1 60); do
  if curl -fsS --noproxy "*" --max-time 2 "${DAEMON_URL}/api/v1/health" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done
if ! curl -fsS --noproxy "*" --max-time 2 "${DAEMON_URL}/api/v1/health" >/dev/null 2>&1; then
  echo "agentd did not become healthy: ${DAEMON_URL}" >&2
  exit 1
fi

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

