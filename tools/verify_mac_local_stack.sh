#!/usr/bin/env bash
set -euo pipefail

# macOS local verification (agentd + WebUI, no Docker).
# - Builds agentd (if needed), starts it locally, checks /api/v1/health
# - Builds WebUI and serves it via a local static server, checks "/"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/lib/agent_env.sh
source "${ROOT}/tools/lib/agent_env.sh"
# shellcheck source=tools/lib/python_helpers.sh
source "${ROOT}/tools/lib/python_helpers.sh"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${ROOT}/tests/lib/agentd_smoke_lib.sh"
curl() {
  agentd_smoke_curl "$@"
}

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
LOG_DIAGNOSTICS="${LOG_DIR}/mac_local_diagnostics_${ts}.log"
LOG_PROVIDER_TESTS="${LOG_DIR}/mac_local_provider_tests_${ts}.log"

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
AGENTD_TOOLS="${AGENTD_TOOLS:-none}"
STATE_DIR="${AGENTD_STATE_DIR:-${ROOT}/state/mac_local}"

WEBUI_PORT="${WEBUI_PORT:-$(pick_port)}"
MAC_LOCAL_SKIP_UI="${MAC_LOCAL_SKIP_UI:-0}"
MAC_LOCAL_UI_INSTALL="${MAC_LOCAL_UI_INSTALL:-1}"
MAC_LOCAL_PROVIDER_TEST="${MAC_LOCAL_PROVIDER_TEST:-0}"
MAC_LOCAL_PROVIDER_TEST_TIMEOUT_MS="${MAC_LOCAL_PROVIDER_TEST_TIMEOUT_MS:-30000}"

cleanup() {
  if [[ -n "${WEBUI_PID:-}" ]]; then
    kill "${WEBUI_PID}" >/dev/null 2>&1 || true
  fi
  if [[ -n "${AGENTD_PID:-}" ]]; then
    kill "${AGENTD_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

if [[ "${MAC_LOCAL_PROVIDER_TEST}" == "1" ]]; then
  agent_env_source_home_if_unset >/dev/null 2>&1 || true
  if [[ "${AGENTD_TOOLS}" == "none" ]]; then
    AGENTD_TOOLS="basic"
    echo "[mac-local] provider tests enabled: switching agentd tools to ${AGENTD_TOOLS}"
  fi
fi

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
  --tools "${AGENTD_TOOLS}" \
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

diag_json="$(curl -fsS -H "Authorization: Bearer ${AGENTD_AUTH_TOKEN}" \
  "http://127.0.0.1:${AGENTD_PORT}/api/v1/diagnostics")"
providers_json="$(curl -fsS -H "Authorization: Bearer ${AGENTD_AUTH_TOKEN}" \
  "http://127.0.0.1:${AGENTD_PORT}/api/v1/diagnostics/providers")"

DIAG_JSON="${diag_json}" PROVIDERS_JSON="${providers_json}" python3 - <<'PY' >"${LOG_DIAGNOSTICS}" 2>&1
import json, os, sys
diag = json.loads(os.environ["DIAG_JSON"])
if not diag.get("ok"):
  print("diagnostics not ok:", diag, file=sys.stderr)
  raise SystemExit(1)
providers = json.loads(os.environ["PROVIDERS_JSON"])
if not providers.get("ok"):
  print("providers not ok:", providers, file=sys.stderr)
  raise SystemExit(1)
prov = providers.get("providers")
if not isinstance(prov, dict):
  print("providers missing or not object", file=sys.stderr)
  raise SystemExit(1)
for name in ("deepseek", "moonshot", "openrouter", "openai"):
  if name not in prov:
    print("missing provider entry:", name, file=sys.stderr)
    raise SystemExit(1)
print("diagnostics ok")
PY

if [[ "${MAC_LOCAL_PROVIDER_TEST}" == "1" ]]; then
  echo "[mac-local] provider tests enabled (log: ${LOG_PROVIDER_TESTS})"
  touch "${LOG_PROVIDER_TESTS}"
  run_provider_test() {
    local provider="$1"
    local prompt="$2"
    local expect="$3"
    local payload
    payload="$(python3 - <<PY
import json
print(json.dumps({
  "provider": "${provider}",
  "prompt": "${prompt}",
  "expect": "${expect}",
  "tools": "basic",
  "require_tool_call": True,
  "timeout_ms": int("${MAC_LOCAL_PROVIDER_TEST_TIMEOUT_MS}"),
  "max_steps": 6
}))
PY
)"
    local resp
    resp="$(printf '%s' "${payload}" | curl -fsS --max-time 240 \
      -H "Authorization: Bearer ${AGENTD_AUTH_TOKEN}" \
      -H "Content-Type: application/json" \
      -d @- \
      "http://127.0.0.1:${AGENTD_PORT}/api/v1/diagnostics/provider_test")"
    PROVIDER_NAME="${provider}" PROVIDER_RESP="${resp}" python3 - <<'PY' >>"${LOG_PROVIDER_TESTS}" 2>&1
import json, os, sys
provider = os.environ.get("PROVIDER_NAME", "")
obj = json.loads(os.environ["PROVIDER_RESP"])
ok = bool(obj.get("ok"))
model = obj.get("model") or ""
base_url = obj.get("base_url") or ""
duration = obj.get("duration_ms")
print(f"[provider_test] provider={provider} ok={ok} duration_ms={duration} model={model} base_url={base_url}")
if not ok:
  print("provider test failed:", obj, file=sys.stderr)
  raise SystemExit(1)
PY
  }

  if [[ -n "${DEEPSEEK_API_KEY:-}" ]]; then
    run_provider_test "deepseek" "Use the calculator tool to compute (2+2)*10. Return exactly: 40" "40"
  else
    echo "[mac-local] skip deepseek provider_test (DEEPSEEK_API_KEY not set)" >>"${LOG_PROVIDER_TESTS}"
  fi

  if [[ -n "${KIMI_API_KEY_CN:-}" || -n "${MOONSHOT_API_KEY:-}" || -n "${MOONSHOT_API_KEY_CN:-}" ]]; then
    run_provider_test "moonshot" "Use the calculator tool to compute (2+2)*10. Return exactly: 40" "40"
  else
    echo "[mac-local] skip moonshot provider_test (KIMI_API_KEY_CN/MOONSHOT_API_KEY not set)" >>"${LOG_PROVIDER_TESTS}"
  fi
fi

if [[ "${MAC_LOCAL_SKIP_UI}" == "1" ]]; then
  echo "[mac-local] skip WebUI build (MAC_LOCAL_SKIP_UI=1)"
  echo "[mac-local] OK"
  echo "  - agentd: http://127.0.0.1:${AGENTD_PORT}/api/v1/health (auth: ${AGENTD_AUTH_TOKEN})"
  exit 0
fi

if ! command -v npm >/dev/null 2>&1; then
  echo "[mac-local] npm not found; skipping WebUI build" >&2
  echo "[mac-local] OK"
  echo "  - agentd: http://127.0.0.1:${AGENTD_PORT}/api/v1/health (auth: ${AGENTD_AUTH_TOKEN})"
  exit 0
fi

if ! command -v python3 >/dev/null 2>&1 && ! command -v python >/dev/null 2>&1; then
  echo "[mac-local] python not found; skipping WebUI serve" >&2
  echo "[mac-local] OK"
  echo "  - agentd: http://127.0.0.1:${AGENTD_PORT}/api/v1/health (auth: ${AGENTD_AUTH_TOKEN})"
  exit 0
fi

if [[ "${MAC_LOCAL_UI_INSTALL}" == "1" ]]; then
  echo "[mac-local] building WebUI (logs: ${LOG_WEBUI_INSTALL}, ${LOG_WEBUI_BUILD})"
  (cd "${ROOT}/ui" && NPM_CONFIG_CACHE=./.npm-cache npm ci) >"${LOG_WEBUI_INSTALL}" 2>&1
else
  echo "[mac-local] WebUI install skipped (MAC_LOCAL_UI_INSTALL=0)"
fi

if [[ ! -d "${ROOT}/ui/node_modules" ]]; then
  echo "[mac-local] UI deps missing; skipping WebUI build" >&2
  echo "[mac-local] OK"
  echo "  - agentd: http://127.0.0.1:${AGENTD_PORT}/api/v1/health (auth: ${AGENTD_AUTH_TOKEN})"
  exit 0
fi

(cd "${ROOT}/ui" && NPM_CONFIG_CACHE=./.npm-cache npm run build) >"${LOG_WEBUI_BUILD}" 2>&1

echo "[mac-local] serving WebUI on 127.0.0.1:${WEBUI_PORT}"
http_server_cmd="$(python_http_server_cmd "${WEBUI_PORT}")"
(cd "${ROOT}/ui/dist" && ${http_server_cmd} >/dev/null 2>&1) &
WEBUI_PID=$!

sleep 1
curl -fsS "http://127.0.0.1:${WEBUI_PORT}/" >/dev/null

echo "[mac-local] OK"
echo "  - WebUI:  http://127.0.0.1:${WEBUI_PORT}/"
echo "  - agentd: http://127.0.0.1:${AGENTD_PORT}/api/v1/health (auth: ${AGENTD_AUTH_TOKEN})"
