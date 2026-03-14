#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${ROOT}/tests/lib/agentd_smoke_lib.sh"
curl() {
  agentd_smoke_curl "$@"
}

ts="$(date +%Y%m%d_%H%M%S)"
log_dir="${ROOT}/build/e2e"
mkdir -p "${log_dir}"

ui_log="${log_dir}/ui_dev_${ts}.log"
pw_install_log="${log_dir}/playwright_install_${ts}.log"
pw_log="${log_dir}/playwright_${ts}.log"

cleanup() {
  set +e
  if [[ -n "${UI_PID:-}" ]]; then kill "${UI_PID}" >/dev/null 2>&1 || true; fi
}
trap cleanup EXIT

if [[ -z "${AGENT_E2E_UI_BASE_URL:-}" ]]; then
  if [[ -n "${AGENT_UI_PORT:-}" ]]; then
    PORT="${AGENT_UI_PORT}"
  else
    PORT="$(python - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
port = s.getsockname()[1]
s.close()
print(port)
PY
)"
  fi
  AGENT_E2E_UI_BASE_URL="http://127.0.0.1:${PORT}"

  echo "[playwright] starting ui dev server on ${AGENT_E2E_UI_BASE_URL} (log: ${ui_log})"
  (cd ui && npm run dev -- --host 127.0.0.1 --port "${PORT}" --strictPort) >"${ui_log}" 2>&1 &
  UI_PID="$!"

  ready=0
  for _ in {1..120}; do
    if curl -fsS "${AGENT_E2E_UI_BASE_URL}" >/dev/null 2>&1; then
      ready=1
      break
    fi
    sleep 0.25
  done
  if [[ "${ready}" != "1" ]]; then
    echo "[playwright] UI did not start. See ${ui_log}" >&2
    exit 1
  fi
else
  echo "[playwright] using existing UI at ${AGENT_E2E_UI_BASE_URL}"
fi

echo "[playwright] ensuring chromium is installed (log: ${pw_install_log})"
(cd ui && npx playwright install chromium) >"${pw_install_log}" 2>&1

echo "[playwright] running smoke specs (log: ${pw_log})"
AGENT_E2E_UI_BASE_URL="${AGENT_E2E_UI_BASE_URL}" \
  bash -lc "cd ui && npx playwright test \
    e2e/agentd_host_smoke.spec.ts \
    e2e/workflow_schedules.spec.ts \
    e2e/broker_console.spec.ts \
    e2e/broker_membership_flow.spec.ts \
    e2e/broker_trace_lookup.spec.ts \
    e2e/workflow_graph_editor.spec.ts \
    --config playwright.smoke.config.ts" >"${pw_log}" 2>&1

echo "[playwright] OK"
echo "[playwright] logs: ${pw_log}"
echo "[playwright] artifacts: ${ROOT}/ui/test-results/"
