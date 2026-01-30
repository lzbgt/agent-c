#!/usr/bin/env bash
set -euo pipefail

AGENTD_BIN="${1:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "missing agentd binary path arg" >&2
  exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
LOG_DIR="${PROJECT_ROOT}/build"
mkdir -p "${LOG_DIR}"

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
  rm -f "${PROJECT_ROOT}/build/agentd_file_symlink_out" >/dev/null 2>&1 || true
  rm -rf "${TMPDIR_CREATED:-}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

TMPDIR_CREATED="$(mktemp -d)"
echo "SECRET_FILE" > "${TMPDIR_CREATED}/secret.txt"

# Create a symlink inside the daemon host scope (PROJECT_ROOT) pointing outside.
ln -s "${TMPDIR_CREATED}" "${PROJECT_ROOT}/build/agentd_file_symlink_out" 2>/dev/null || true
if [[ ! -L "${PROJECT_ROOT}/build/agentd_file_symlink_out" ]]; then
  echo "SKIP: cannot create symlink in this environment" >&2
  exit 77
fi

"${AGENTD_BIN}" \
  --host "${HOST}" \
  --port "${PORT}" \
  --no-yolo \
  --tools host \
  > "${LOG_DIR}/agentd_file_symlink_smoke.stdout.log" 2> "${LOG_DIR}/agentd_file_symlink_smoke.stderr.log" &
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

# Scoped file endpoint must reject reading through symlink escapes.
code="$(curl -sS --noproxy "*" --max-time 5 -o /dev/null -w '%{http_code}' \
  "${DAEMON_URL}/api/v1/file?yolo=0&path=$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote('build/agentd_file_symlink_out/secret.txt'))
PY
)")"

if [[ "${code}" != "403" && "${code}" != "404" ]]; then
  echo "expected 403/404 for symlink escape file read, got ${code}" >&2
  exit 1
fi

