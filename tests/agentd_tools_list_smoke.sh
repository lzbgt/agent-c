#!/usr/bin/env bash
set -euo pipefail

AGENTD_BIN="${1:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "missing agentd binary path arg" >&2
  exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

PORT="$(( (RANDOM % 20000) + 20000 ))"
HOST="127.0.0.1"
DAEMON_URL="http://${HOST}:${PORT}"

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

LOG_DIR="${PROJECT_ROOT}/build"
mkdir -p "${LOG_DIR}"

"${AGENTD_BIN}" \
  --host "${HOST}" \
  --port "${PORT}" \
  --tools host \
  > "${LOG_DIR}/agentd_tools_list_smoke.stdout.log" 2> "${LOG_DIR}/agentd_tools_list_smoke.stderr.log" &
AGENTD_PID=$!

# Wait for health endpoint.
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

resp="$(curl -fsS --noproxy "*" --max-time 5 "${DAEMON_URL}/api/v1/tools?tools=host&yolo=1")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp}''')
if not obj.get("ok"):
  print(obj, file=sys.stderr)
  raise SystemExit(1)
defs = obj.get("defs") or []
names = {d.get("name") for d in defs if isinstance(d, dict)}
need = {"shell_exec", "proc_exec", "file_apply_patch", "fs_read", "fs_list", "fs_stat"}
missing = sorted(need - names)
if missing:
  print("missing tools:", missing, file=sys.stderr)
  raise SystemExit(1)
PY

