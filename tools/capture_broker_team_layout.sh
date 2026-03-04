#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${OUT_DIR:-${ROOT_DIR}/out}"
PORT="${PORT:-5179}"
TS="$(date +%Y%m%d_%H%M%S)"
UI_LOG="${OUT_DIR}/ui_dev_${TS}.log"
PW_LOG="${OUT_DIR}/playwright_broker_team_layout_${TS}.log"

mkdir -p "${OUT_DIR}"

cleanup() {
  if [[ -n "${DEV_PID:-}" ]]; then
    kill "${DEV_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

cd "${ROOT_DIR}/ui"

npm run dev -- --host 127.0.0.1 --port "${PORT}" >"${UI_LOG}" 2>&1 &
DEV_PID=$!

for _ in {1..60}; do
  if curl -sf "http://127.0.0.1:${PORT}" >/dev/null 2>&1; then
    break
  fi
  sleep 1
 done

AGENT_E2E_UI_BASE_URL="http://127.0.0.1:${PORT}" \
AGENT_E2E_SCREENSHOT_OUT="${OUT_DIR}" \
  npx playwright test e2e/broker_team_layout.spec.ts --config playwright.smoke.config.ts >"${PW_LOG}" 2>&1

echo "UI log: ${UI_LOG}"
echo "Playwright log: ${PW_LOG}"
echo "Screenshot: ${OUT_DIR}/broker_team_layout.png"
