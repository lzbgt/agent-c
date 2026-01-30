#!/usr/bin/env bash
set -euo pipefail

# Real end-to-end harness:
# - starts agentd (real provider key loaded from .not_in_repo or env)
# - starts ui dev server
# - runs Playwright against the running UI
#
# This is intentionally NOT part of ctest: it depends on external LLM providers and local browser runtime.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

ts="$(date +%Y%m%d_%H%M%S)"
log_dir="${ROOT}/build/e2e"
mkdir -p "${log_dir}"

agentd_log="${log_dir}/agentd_${ts}.log"
ui_log="${log_dir}/ui_${ts}.log"
e2e_log="${log_dir}/playwright_${ts}.log"

cleanup() {
  set +e
  if [[ -n "${UI_PID:-}" ]]; then kill "${UI_PID}" >/dev/null 2>&1 || true; fi
  if [[ -n "${AGENTD_PID:-}" ]]; then kill "${AGENTD_PID}" >/dev/null 2>&1 || true; fi
}
trap cleanup EXIT

if [[ ! -x "${ROOT}/build/agentd" ]]; then
  echo "[e2e] build/agentd missing; run tools/verify.sh first" >&2
  exit 2
fi

# Start agentd. If you want daemon auth, set AGENTD_AUTH_TOKEN and update UI settings accordingly.
echo "[e2e] starting agentd (log: ${agentd_log})"
"${ROOT}/build/agentd" >"${agentd_log}" 2>&1 &
AGENTD_PID="$!"

# Start UI dev server.
echo "[e2e] starting ui dev server (log: ${ui_log})"
(cd ui && npm run dev -- --host 127.0.0.1 --port 5173) >"${ui_log}" 2>&1 &
UI_PID="$!"

echo "[e2e] waiting for ui to be reachable..."
for i in {1..120}; do
  if curl -fsS "http://127.0.0.1:5173" >/dev/null 2>&1; then
    break
  fi
  sleep 0.25
done

echo "[e2e] running playwright (log: ${e2e_log})"
(cd ui && npm run e2e) >"${e2e_log}" 2>&1

echo "[e2e] OK"

