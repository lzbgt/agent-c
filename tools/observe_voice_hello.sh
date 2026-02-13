#!/usr/bin/env bash
set -euo pipefail

# Open-world E2E observation harness (local, production-like):
# - starts agentd + webui dev server on free ports
# - runs a Playwright "observation" flow with the exact prompt: "say hello in voice to me"
# - captures screenshots, console logs, network failures, and a DB snapshot for postmortem
#
# IMPORTANT:
# - sources ~/.env if present (for real LLM provider keys like DEEPSEEK_API_KEY)
# - does NOT print secrets

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${ROOT}/out"
mkdir -p "${OUT_DIR}"

if [[ -f "${HOME}/.env" ]]; then
  set -a
  # shellcheck disable=SC1090
  source "${HOME}/.env"
  set +a
fi

pick_port() {
  local preferred="$1"
  python3 - "${preferred}" <<'PY'
import socket, sys
preferred = int(sys.argv[1])

def free(p: int) -> bool:
  # Check IPv4 and IPv6 loopback
  for host, fam in [("127.0.0.1", socket.AF_INET), ("::1", socket.AF_INET6)]:
    try:
      s = socket.socket(fam, socket.SOCK_STREAM)
    except Exception:
      continue
    try:
      s.settimeout(0.15)
      if s.connect_ex((host, p)) == 0:
        return False
    finally:
      try: s.close()
      except Exception: pass
  return True

for p in [preferred, preferred+1, preferred+2, preferred+10, preferred+100]:
  if free(p):
    print(p)
    raise SystemExit(0)
print(preferred+1000)
PY
}

AGENTD_PORT="${AGENTD_PORT:-$(pick_port 8123)}"
UI_PORT="${UI_PORT:-$(pick_port 5173)}"

RUN_ID="voice_hello_$(date +%Y%m%d_%H%M%S)"
RUN_DIR="${OUT_DIR}/${RUN_ID}"
mkdir -p "${RUN_DIR}"

AGENTD_BIN="${ROOT}/build/agentd"
if [[ ! -x "${AGENTD_BIN}" ]]; then
  echo "[observe] ERROR: missing agentd binary at ${AGENTD_BIN}. Build first." >&2
  exit 2
fi

STATE_DIR="${RUN_DIR}/state"
SESSIONS_ROOT="${STATE_DIR}/sessions"
DB_PATH="${RUN_DIR}/agentd.sqlite"
mkdir -p "${SESSIONS_ROOT}"

AGENTD_LOG="${RUN_DIR}/agentd.log"
UI_LOG="${RUN_DIR}/webui.log"
PW_LOG="${RUN_DIR}/playwright.log"

AGENTD_BASE="http://127.0.0.1:${AGENTD_PORT}"
UI_BASE="http://127.0.0.1:${UI_PORT}"

echo "[observe] run_dir=${RUN_DIR}"
echo "[observe] agentd=${AGENTD_BASE}"
echo "[observe] webui=${UI_BASE}"

cleanup() {
  set +e
  if [[ -n "${UI_PID:-}" ]]; then kill "${UI_PID}" >/dev/null 2>&1 || true; fi
  if [[ -n "${AGENTD_PID:-}" ]]; then kill "${AGENTD_PID}" >/dev/null 2>&1 || true; fi
  sleep 0.2
}
trap cleanup EXIT

echo "[observe] starting agentd..."
(
  cd "${ROOT}"
  "${AGENTD_BIN}" \
    --host 127.0.0.1 \
    --port "${AGENTD_PORT}" \
    --state-dir "${STATE_DIR}" \
    --sessions-root "${SESSIONS_ROOT}" \
    --db-path "${DB_PATH}" \
    --tools host \
    --tools-root "${ROOT}" \
    --yolo \
    --cors-origin "*" \
    >"${AGENTD_LOG}" 2>&1
) &
AGENTD_PID="$!"

echo "[observe] starting webui dev server..."
(
  cd "${ROOT}/ui"
  npm run dev -- --host 127.0.0.1 --port "${UI_PORT}" >"${UI_LOG}" 2>&1
) &
UI_PID="$!"

wait_http_ok() {
  local url="$1"
  local timeout_s="${2:-120}"
  python3 - "${url}" "${timeout_s}" <<'PY'
import sys, time, urllib.request
url = sys.argv[1]
timeout_s = int(sys.argv[2])
deadline = time.time() + timeout_s
while time.time() < deadline:
  try:
    with urllib.request.urlopen(url, timeout=2) as r:
      if r.status >= 200 and r.status < 500:
        print("ok")
        raise SystemExit(0)
  except Exception:
    time.sleep(0.5)
print("timeout")
raise SystemExit(1)
PY
}

echo "[observe] waiting for agentd health..."
wait_http_ok "${AGENTD_BASE}/api/v1/health" 120 >/dev/null
echo "[observe] waiting for webui..."
wait_http_ok "${UI_BASE}/" 120 >/dev/null

echo "[observe] running Playwright observation..."
(
  cd "${ROOT}/ui"
  AGENT_E2E_UI_BASE_URL="${UI_BASE}" \
  AGENT_E2E_AGENTD_BASE_URL="${AGENTD_BASE}" \
  AGENT_E2E_OUT_DIR="${RUN_DIR}" \
  AGENT_E2E_REQUIRE_VOICE=1 \
  npx playwright test e2e/observe_voice_hello.spec.ts
) >"${PW_LOG}" 2>&1 || true

echo "[observe] done. artifacts:"
echo "  - ${AGENTD_LOG}"
echo "  - ${UI_LOG}"
echo "  - ${PW_LOG}"
echo "  - ${DB_PATH}"
echo "  - ${RUN_DIR}/page.png (if captured)"
