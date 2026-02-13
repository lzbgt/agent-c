#!/usr/bin/env bash
set -euo pipefail

# macOS local verification (agentd + WebUI, no Docker).
# - Builds agentd (if needed), starts it locally, checks /api/v1/health
# - Builds WebUI and serves it via a local static server, checks "/"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "[mac-local] WARNING: not running on macOS; continuing anyway"
fi

LOG_DIR="${ROOT}/out"
mkdir -p "${LOG_DIR}"
ts="$(date +%Y-%m-%d_%H%M%S)"
LOG_CMAKE="${LOG_DIR}/mac_local_cmake_configure_${ts}.log"
LOG_AGENTD="${LOG_DIR}/mac_local_agentd_${ts}.log"
LOG_WEBUI_INSTALL="${LOG_DIR}/mac_local_webui_install_${ts}.log"
LOG_WEBUI_BUILD="${LOG_DIR}/mac_local_webui_build_${ts}.log"

pick_port() {
  python3 - <<'PY'
import socket
s=socket.socket()
s.bind(('127.0.0.1',0))
print(s.getsockname()[1])
s.close()
PY
}

AGENTD_BIN="${AGENTD_BIN:-${ROOT}/build/agentd}"
AGENTD_PORT="${AGENTD_PORT:-$(pick_port)}"
AGENTD_AUTH_TOKEN="${AGENTD_AUTH_TOKEN:-dev-agentd-token}"
STATE_DIR="${AGENTD_STATE_DIR:-${ROOT}/state/mac_local}"

WEBUI_PORT="${WEBUI_PORT:-$(pick_port)}"

cleanup() {
  if [[ -n "${WEBUI_PID:-}" ]]; then
    kill "${WEBUI_PID}" >/dev/null 2>&1 || true
  fi
  if [[ -n "${AGENTD_PID:-}" ]]; then
    kill "${AGENTD_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

if [[ ! -x "${AGENTD_BIN}" ]]; then
  if [[ ! -f "${ROOT}/build/CMakeCache.txt" ]]; then
    echo "[mac-local] cmake configure (log: ${LOG_CMAKE})"
    cmake -S "${ROOT}" -B "${ROOT}/build" >"${LOG_CMAKE}" 2>&1
  fi
  echo "[mac-local] agentd not found; building (log: ${LOG_AGENTD})"
  cmake --build "${ROOT}/build" >"${LOG_AGENTD}" 2>&1
fi

mkdir -p "${STATE_DIR}"
echo "[mac-local] starting agentd on 127.0.0.1:${AGENTD_PORT} (log: ${LOG_AGENTD})"
"${AGENTD_BIN}" \
  --host 127.0.0.1 \
  --port "${AGENTD_PORT}" \
  --auth-token "${AGENTD_AUTH_TOKEN}" \
  --tools none \
  --state-dir "${STATE_DIR}" \
  --db-path "${STATE_DIR}/agentd.db" >"${LOG_AGENTD}" 2>&1 &
AGENTD_PID=$!

echo "[mac-local] waiting for agentd health..."
for _ in $(seq 1 30); do
  if curl -fsS -H "Authorization: Bearer ${AGENTD_AUTH_TOKEN}" \
    "http://127.0.0.1:${AGENTD_PORT}/api/v1/health" >/dev/null 2>&1; then
    break
  fi
  sleep 1
done
curl -fsS -H "Authorization: Bearer ${AGENTD_AUTH_TOKEN}" \
  "http://127.0.0.1:${AGENTD_PORT}/api/v1/health" | python3 -m json.tool >/dev/null

echo "[mac-local] building WebUI (logs: ${LOG_WEBUI_INSTALL}, ${LOG_WEBUI_BUILD})"
(cd "${ROOT}/ui" && NPM_CONFIG_CACHE=./.npm-cache npm ci) >"${LOG_WEBUI_INSTALL}" 2>&1
(cd "${ROOT}/ui" && NPM_CONFIG_CACHE=./.npm-cache npm run build) >"${LOG_WEBUI_BUILD}" 2>&1

echo "[mac-local] serving WebUI on 127.0.0.1:${WEBUI_PORT}"
(cd "${ROOT}/ui/dist" && python3 -m http.server "${WEBUI_PORT}" >/dev/null 2>&1) &
WEBUI_PID=$!

sleep 1
curl -fsS "http://127.0.0.1:${WEBUI_PORT}/" >/dev/null

echo "[mac-local] OK"
echo "  - WebUI:  http://127.0.0.1:${WEBUI_PORT}/"
echo "  - agentd: http://127.0.0.1:${AGENTD_PORT}/api/v1/health (auth: ${AGENTD_AUTH_TOKEN})"
