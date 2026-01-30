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
HOST="0.0.0.0"
URL_LOCAL="http://127.0.0.1:${PORT}"

set +e
"${AGENTD_BIN}" --host "${HOST}" --port "${PORT}" --tools host \
  > "${LOG_DIR}/agentd_non_loopback_guard.stdout.log" 2> "${LOG_DIR}/agentd_non_loopback_guard.stderr.log"
rc=$?
set -e
if [[ "${rc}" -eq 0 ]]; then
  echo "expected non-loopback unauth agentd to fail, got rc=0" >&2
  exit 1
fi

# With explicit override, daemon should start.
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

"${AGENTD_BIN}" --host "${HOST}" --port "${PORT}" --tools host --allow-unauth \
  > "${LOG_DIR}/agentd_non_loopback_allow.stdout.log" 2> "${LOG_DIR}/agentd_non_loopback_allow.stderr.log" &
AGENTD_PID=$!

for _ in $(seq 1 60); do
  if curl -fsS --noproxy "*" --max-time 2 "${URL_LOCAL}/api/v1/health" >/dev/null 2>&1; then
    exit 0
  fi
  sleep 0.1
done

echo "agentd did not become healthy (allow-unauth): ${URL_LOCAL}" >&2
exit 1

