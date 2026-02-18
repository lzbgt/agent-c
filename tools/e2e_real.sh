#!/usr/bin/env bash
set -euo pipefail

# Real end-to-end harness:
# - starts agentd (real provider key loaded from .not_in_repo or env)
# - starts ui dev server
# - runs Playwright against the running UI
#
# This is intentionally NOT part of ctest: it depends on external LLM providers and local browser runtime.
# To enable the voice observation test, set AGENT_E2E_REQUIRE_VOICE=1 and provide a voice-capable provider.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

ts="$(date +%Y%m%d_%H%M%S)"
log_dir="${ROOT}/build/e2e"
mkdir -p "${log_dir}"

agentd_log="${log_dir}/agentd_${ts}.log"
ui_log="${log_dir}/ui_${ts}.log"
e2e_log="${log_dir}/playwright_${ts}.log"
pw_install_log="${log_dir}/playwright_install_${ts}.log"
state_dir="${log_dir}/state_${ts}"

# Best-effort: load local env keys (user requested `source ~/.env`).
# Keep it silent and do not echo any variables.
# shellcheck source=tools/lib/agent_env.sh
source "${ROOT}/tools/lib/agent_env.sh"
agent_env_source_home_if_unset >/dev/null 2>&1 || true

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

# Provider key check (best-effort).
# We do not print keys; we just detect whether one exists.
if [[ -z "${DEEPSEEK_API_KEY:-}" && -z "${OPENROUTER_API_KEY:-}" && -z "${OPENAI_API_KEY:-}" ]]; then
  if [[ ! -f "${ROOT}/.not_in_repo" && ! -f "${ROOT}/project.local.md" ]]; then
    echo "[e2e] Missing provider keys. Create ${ROOT}/.not_in_repo (gitignored) or set env DEEPSEEK_API_KEY/OPENROUTER_API_KEY/OPENAI_API_KEY." >&2
    echo "[e2e] See README.md section 'Local secrets file: .not_in_repo'." >&2
    exit 2
  fi
fi

# Start agentd. If you want daemon auth, set AGENTD_AUTH_TOKEN and update UI settings accordingly.
echo "[e2e] starting agentd (log: ${agentd_log})"
mkdir -p "${state_dir}"
"${ROOT}/build/agentd" --state-dir "${state_dir}" >"${agentd_log}" 2>&1 &
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

echo "[e2e] ensuring Playwright browsers are installed (log: ${pw_install_log})"
(cd ui && npx playwright install chromium) >"${pw_install_log}" 2>&1 || {
  echo "[e2e] Playwright install failed. See: ${pw_install_log}" >&2
  exit 1
}

echo "[e2e] running playwright (log: ${e2e_log})"
(cd ui && npm run e2e) >"${e2e_log}" 2>&1 || {
  echo "[e2e] FAILED. See logs:" >&2
  echo "  - ${agentd_log}" >&2
  echo "  - ${ui_log}" >&2
  echo "  - ${pw_install_log}" >&2
  echo "  - ${e2e_log}" >&2
  exit 1
}

echo "[e2e] OK"
echo "[e2e] artifacts: ${ROOT}/ui/test-results/"
