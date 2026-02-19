#!/usr/bin/env bash
set -euo pipefail

# One-command dev stack (agentd + broker + connector + WebUI) on macOS host.
# - Runs Postgres + Keycloak via docker compose (no build)
# - Runs agentd + broker + connector + WebUI on the host
# - Performs smoke checks + captures evidence bundle

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<'USAGE'
Usage: tools/devstack_agent.sh [options]

Options:
  --keep           Keep services running after checks (default).
  --no-keep        Stop services after checks.
  --skip-ui        Skip WebUI build/serve.
  --ui-install     Run npm ci before building UI (default: 1).
  --agentd-tools   agentd tools mode (default: none)
  --agentd-port    agentd port (default: random)
  --broker-port    broker port (default: random)
  --webui-port     WebUI port (default: random)
  --postgres-port  Postgres published port (default: random)
  --keycloak-port  Keycloak published port (default: random)
  --out-dir        Output directory for logs/state (default: out/devstack_<ts>)
  --state-dir      agentd state dir (default: state/devstack)
  -h, --help       Show this help
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "[devstack] WARNING: not running on macOS; continuing anyway"
fi

source "${ROOT}/tools/lib/docker_preflight.sh"
# shellcheck source=tools/lib/python_helpers.sh
source "${ROOT}/tools/lib/python_helpers.sh"
if ! docker_compose_preflight "devstack"; then
  exit 77
fi

pick_port() {
  python3 - <<'PY'
import socket
s=socket.socket()
s.bind(('127.0.0.1',0))
print(s.getsockname()[1])
s.close()
PY
}

run_logged() {
  local label="$1"
  local log="$2"
  shift 2
  echo "[devstack] ${label}"
  if ! "$@" >"${log}" 2>&1; then
    echo "[devstack] FAILED: ${label} (log: ${log})" >&2
    tail -n 120 "${log}" >&2 || true
    return 1
  fi
  echo "[devstack] OK: ${label} (log: ${log})"
}

KEEP=1
SKIP_UI=0
UI_INSTALL=1
AGENTD_TOOLS="none"
AGENTD_PORT=""
BROKER_PORT=""
WEBUI_PORT=""
POSTGRES_PORT=""
KEYCLOAK_PORT=""
OUT_DIR=""
AGENTD_STATE_DIR=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --keep) KEEP=1; shift ;;
    --no-keep) KEEP=0; shift ;;
    --skip-ui) SKIP_UI=1; shift ;;
    --ui-install) UI_INSTALL=1; shift ;;
    --agentd-tools) AGENTD_TOOLS="$2"; shift 2 ;;
    --agentd-port) AGENTD_PORT="$2"; shift 2 ;;
    --broker-port) BROKER_PORT="$2"; shift 2 ;;
    --webui-port) WEBUI_PORT="$2"; shift 2 ;;
    --postgres-port) POSTGRES_PORT="$2"; shift 2 ;;
    --keycloak-port) KEYCLOAK_PORT="$2"; shift 2 ;;
    --out-dir) OUT_DIR="$2"; shift 2 ;;
    --state-dir) AGENTD_STATE_DIR="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown arg: $1" >&2; usage; exit 1 ;;
  esac
done

AGENTD_BIN="${AGENTD_BIN:-${ROOT}/build/agentd}"
BROKER_BIN="${BROKER_BIN:-${ROOT}/out/devstack/bin/agentd-broker}"
CONNECTOR_BIN="${CONNECTOR_BIN:-${ROOT}/out/devstack/bin/agentd-connector}"
AGENTD_AUTH_TOKEN="${AGENTD_AUTH_TOKEN:-dev-agentd-token}"
BROKER_AGENT_ID="${BROKER_AGENT_ID:-agent1}"

AGENTD_PORT="${AGENTD_PORT:-$(pick_port)}"
BROKER_PORT="${BROKER_PORT:-$(pick_port)}"
WEBUI_PORT="${WEBUI_PORT:-$(pick_port)}"
POSTGRES_PORT="${POSTGRES_PORT:-$(pick_port)}"
KEYCLOAK_PORT="${KEYCLOAK_PORT:-$(pick_port)}"

AGENTD_STATE_DIR="${AGENTD_STATE_DIR:-${ROOT}/state/devstack}"

TS="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="${OUT_DIR:-${ROOT}/out/devstack_${TS}}"
LOG_DIR="${OUT_DIR}/logs"
mkdir -p "${LOG_DIR}"

LOG_CMAKE_CONFIG="${LOG_DIR}/cmake_config.log"
LOG_CMAKE_BUILD="${LOG_DIR}/cmake_build.log"
LOG_AGENTD="${LOG_DIR}/agentd.log"
LOG_BROKER_BUILD="${LOG_DIR}/broker_build.log"
LOG_CONNECTOR_BUILD="${LOG_DIR}/connector_build.log"
LOG_BROKER="${LOG_DIR}/broker.log"
LOG_CONNECTOR="${LOG_DIR}/connector.log"
LOG_WEBUI_INSTALL="${LOG_DIR}/webui_install.log"
LOG_WEBUI_BUILD="${LOG_DIR}/webui_build.log"
LOG_WEBUI="${LOG_DIR}/webui.log"
LOG_MTLS="${LOG_DIR}/mtls.log"
LOG_COMPOSE="${LOG_DIR}/compose.log"
LOG_EVIDENCE="${LOG_DIR}/evidence.log"

COMPOSE_PROJECT_NAME="${COMPOSE_PROJECT_NAME:-agent_devstack_${WEBUI_PORT}}"
COMPOSE_FILE="${COMPOSE_FILE:-${ROOT}/docker-compose.yml}"

MTLS_DIR="${ROOT}/out/devstack/mtls"
BIN_DIR="${ROOT}/out/devstack/bin"
mkdir -p "${MTLS_DIR}" "${BIN_DIR}" "${AGENTD_STATE_DIR}"

CLEAN_ON_EXIT=1

cleanup() {
  local exit_code="$1"
  if [[ "${exit_code}" -ne 0 ]]; then
    CLEAN_ON_EXIT=1
  fi
  if [[ "${CLEAN_ON_EXIT}" -eq 0 ]]; then
    return 0
  fi
  if [[ -n "${WEBUI_PID:-}" ]]; then
    kill "${WEBUI_PID}" >/dev/null 2>&1 || true
  fi
  if [[ -n "${CONNECTOR_PID:-}" ]]; then
    kill "${CONNECTOR_PID}" >/dev/null 2>&1 || true
  fi
  if [[ -n "${BROKER_PID:-}" ]]; then
    kill "${BROKER_PID}" >/dev/null 2>&1 || true
  fi
  if [[ -n "${AGENTD_PID:-}" ]]; then
    kill "${AGENTD_PID}" >/dev/null 2>&1 || true
  fi
  if docker_compose_preflight "devstack-clean" >/dev/null 2>&1; then
    POSTGRES_PUBLISHED_PORT="${POSTGRES_PORT}" \
    KEYCLOAK_PUBLISHED_PORT="${KEYCLOAK_PORT}" \
    COMPOSE_PROJECT_NAME="${COMPOSE_PROJECT_NAME}" \
    docker compose -f "${COMPOSE_FILE}" down -v --remove-orphans >/dev/null 2>&1 || true
  fi
}
trap 'cleanup $?' EXIT

if ! command -v cmake >/dev/null 2>&1; then
  echo "[devstack] cmake not found" >&2
  exit 1
fi
if ! command -v go >/dev/null 2>&1; then
  echo "[devstack] go not found" >&2
  exit 1
fi

if [[ ! -x "${AGENTD_BIN}" ]]; then
  if [[ ! -f "${ROOT}/build/CMakeCache.txt" ]]; then
    run_logged "cmake configure" "${LOG_CMAKE_CONFIG}" cmake -S "${ROOT}" -B "${ROOT}/build"
  fi
  run_logged "cmake build" "${LOG_CMAKE_BUILD}" cmake --build "${ROOT}/build" -j
fi

run_logged "build broker" "${LOG_BROKER_BUILD}" bash -lc "cd ${ROOT}/broker && go build -trimpath -o ${BROKER_BIN} ./cmd/agentd-broker"
run_logged "build connector" "${LOG_CONNECTOR_BUILD}" bash -lc "cd ${ROOT}/broker && go build -trimpath -o ${CONNECTOR_BIN} ./cmd/agentd-connector"

if [[ "${SKIP_UI}" -eq 0 ]]; then
  if ! command -v npm >/dev/null 2>&1; then
    echo "[devstack] npm not found; skipping WebUI" >&2
    SKIP_UI=1
  fi
fi

if [[ "${SKIP_UI}" -eq 0 ]]; then
  if [[ "${UI_INSTALL}" -eq 1 ]]; then
    run_logged "ui: npm ci" "${LOG_WEBUI_INSTALL}" bash -lc "cd ${ROOT}/ui && NPM_CONFIG_CACHE=./.npm-cache npm ci"
  fi
  if [[ ! -d "${ROOT}/ui/node_modules" ]]; then
    echo "[devstack] UI deps missing; skipping WebUI" >&2
    SKIP_UI=1
  else
    run_logged "ui: npm run build" "${LOG_WEBUI_BUILD}" bash -lc "cd ${ROOT}/ui && NPM_CONFIG_CACHE=./.npm-cache npm run build"
  fi
fi

run_logged "generate mTLS certs" "${LOG_MTLS}" bash -lc "bash ${ROOT}/tools/gen_agentd_broker_mtls_test_certs.sh ${MTLS_DIR} ${BROKER_AGENT_ID}"

compose_env=(
  "POSTGRES_PUBLISHED_PORT=${POSTGRES_PORT}"
  "KEYCLOAK_PUBLISHED_PORT=${KEYCLOAK_PORT}"
  "COMPOSE_PROJECT_NAME=${COMPOSE_PROJECT_NAME}"
)
set +e
env "${compose_env[@]}" docker compose -f "${COMPOSE_FILE}" up -d postgres keycloak --no-build >"${LOG_COMPOSE}" 2>&1
compose_rc=$?
set -e
if [[ "${compose_rc}" -ne 0 ]]; then
  if grep -Eqi "resource temporarily unavailable|runc run failed|containerd" "${LOG_COMPOSE}"; then
    echo "[devstack] SKIP: docker compose failed due to resource limits" >&2
    exit 77
  fi
  echo "[devstack] ERROR: docker compose failed (log: ${LOG_COMPOSE})" >&2
  exit "${compose_rc}"
fi

KEYCLOAK_BASE="http://keycloak.lvh.me:${KEYCLOAK_PORT}"
AGENTD_BASE="http://127.0.0.1:${AGENTD_PORT}"
BROKER_BASE="https://127.0.0.1:${BROKER_PORT}"
WEBUI_BASE="http://127.0.0.1:${WEBUI_PORT}"

wait_http_ok() {
  local url="$1"
  local timeout_s="${2:-240}"
  local started
  started="$(date +%s)"
  while true; do
    if curl -fsS "${url}" >/dev/null 2>&1; then
      return 0
    fi
    local now
    now="$(date +%s)"
    if (( now - started > timeout_s )); then
      echo "[devstack] ERROR: timeout waiting for ${url}" >&2
      return 1
    fi
    sleep 1
  done
}

wait_http_ok "${KEYCLOAK_BASE}/realms/agentd/.well-known/openid-configuration" 240

"${AGENTD_BIN}" \
  --host 127.0.0.1 \
  --port "${AGENTD_PORT}" \
  --auth-token "${AGENTD_AUTH_TOKEN}" \
  --tools "${AGENTD_TOOLS}" \
  --state-dir "${AGENTD_STATE_DIR}" \
  --db-path "${AGENTD_STATE_DIR}/agentd.db" \
  --cors-origin "http://127.0.0.1:${WEBUI_PORT}" \
  --cors-origin "http://localhost:${WEBUI_PORT}" >"${LOG_AGENTD}" 2>&1 &
AGENTD_PID=$!

wait_http_ok "${AGENTD_BASE}/api/v1/health" 240 || true

BROKER_DB_DSN="postgres://postgres:postgres@127.0.0.1:${POSTGRES_PORT}/agentd_broker?sslmode=disable"

"${BROKER_BIN}" \
  --listen "127.0.0.1:${BROKER_PORT}" \
  --tls-cert "${MTLS_DIR}/server.pem" \
  --tls-key "${MTLS_DIR}/server.key.pem" \
  --tls-client-ca "${MTLS_DIR}/ca.pem" \
  --db-dsn "${BROKER_DB_DSN}" \
  --oidc-issuer "${KEYCLOAK_BASE}/realms/agentd" \
  --oidc-audience "agentd-broker-dev" \
  --cors-origins "http://127.0.0.1:${WEBUI_PORT},http://localhost:${WEBUI_PORT}" >"${LOG_BROKER}" 2>&1 &
BROKER_PID=$!

"${CONNECTOR_BIN}" \
  --broker "wss://127.0.0.1:${BROKER_PORT}/v1/agent/connect" \
  --local-agentd "${AGENTD_BASE}" \
  --tls-ca "${MTLS_DIR}/ca.pem" \
  --tls-cert "${MTLS_DIR}/client_${BROKER_AGENT_ID}.pem" \
  --tls-key "${MTLS_DIR}/client_${BROKER_AGENT_ID}.key.pem" \
  --agent-cn-prefix "agentd-" \
  --agent-id "${BROKER_AGENT_ID}" >"${LOG_CONNECTOR}" 2>&1 &
CONNECTOR_PID=$!

if [[ "${SKIP_UI}" -eq 0 ]]; then
  cat <<CFG >"${ROOT}/ui/dist/agentui-config.js"
window.__AGENT_UI_CONFIG__ = {
  connectionMode: "broker",
  brokerBaseUrl: "${BROKER_BASE}",
  brokerAgentId: "${BROKER_AGENT_ID}",
  brokerAuthToken: "",
  daemonAuthToken: "${AGENTD_AUTH_TOKEN}",
  brokerPanelOpen: true,
};
CFG
  if http_server_cmd="$(python_http_server_cmd "${WEBUI_PORT}")"; then
    (cd "${ROOT}/ui/dist" && ${http_server_cmd} >"${LOG_WEBUI}" 2>&1) &
    WEBUI_PID=$!
  else
    echo "[devstack] python not found; skipping WebUI serve" >&2
    SKIP_UI=1
  fi
fi

get_token() {
  local token_json
  token_json="$(
    curl -fsS \
      -d "grant_type=password" \
      -d "client_id=agentd-broker-dev" \
      -d "username=test" \
      -d "password=test" \
      "${KEYCLOAK_BASE}/realms/agentd/protocol/openid-connect/token"
  )"
  echo "${token_json}" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("access_token",""))'
}

OIDC_JWT="$(get_token)"
if [[ -z "${OIDC_JWT}" ]]; then
  echo "[devstack] ERROR: failed to fetch OIDC token" >&2
  exit 2
fi

started="$(date +%s)"
while true; do
  if curl -fsS -k \
    -H "Authorization: Bearer ${OIDC_JWT}" \
    -H "Content-Type: application/json" \
    -d "{\"agent_id\":\"${BROKER_AGENT_ID}\"}" \
    "${BROKER_BASE}/v1/agents" >/dev/null 2>&1; then
    break
  fi
  now="$(date +%s)"
  if (( now - started > 240 )); then
    echo "[devstack] ERROR: broker did not accept create agent in time" >&2
    exit 4
  fi
  sleep 1
 done

started="$(date +%s)"
while true; do
  j="$(curl -fsS -k -H "Authorization: Bearer ${OIDC_JWT}" "${BROKER_BASE}/v1/agents" || true)"
  ok="$(python3 - "${j}" <<'PY'
import json,sys
raw = sys.argv[1] if len(sys.argv) > 1 else ""
try:
  obj = json.loads(raw or "{}")
  for a in (obj.get("agents") or []):
    if a.get("agent_id") == sys.argv[2] and a.get("connected") is True:
      print("yes")
      raise SystemExit(0)
except Exception:
  pass
print("no")
PY
"${BROKER_AGENT_ID}")"
  if [[ "${ok}" == "yes" ]]; then
    break
  fi
  now="$(date +%s)"
  if (( now - started > 240 )); then
    echo "[devstack] ERROR: connector did not become connected in time" >&2
    echo "[devstack] broker /v1/agents response:" >&2
    echo "${j}" >&2
    exit 3
  fi
  sleep 1
 done

curl -fsS -k -H "Authorization: Bearer ${OIDC_JWT}" \
  "${BROKER_BASE}/v1/agents/${BROKER_AGENT_ID}/proxy/api/v1/health" >/dev/null

curl -fsS -H "Authorization: Bearer ${AGENTD_AUTH_TOKEN}" \
  "${AGENTD_BASE}/api/v1/health" >/dev/null

if [[ "${SKIP_UI}" -eq 0 ]]; then
  curl -fsS "${WEBUI_BASE}/" >/dev/null
fi

"${ROOT}/tools/capture_agent_evidence_bundle.sh" \
  --agentd-base "${AGENTD_BASE}" \
  --agentd-token "${AGENTD_AUTH_TOKEN}" \
  --broker-base "${BROKER_BASE}" \
  --broker-token "${OIDC_JWT}" \
  --broker-agent-id "${BROKER_AGENT_ID}" \
  --agentd-via-broker >"${LOG_EVIDENCE}" 2>&1 || true

STATE_PATH="${OUT_DIR}/devstack_state.json"
python3 - <<PY
import json
print(json.dumps({
  "out_dir": "${OUT_DIR}",
  "agentd_base": "${AGENTD_BASE}",
  "broker_base": "${BROKER_BASE}",
  "webui_base": "${WEBUI_BASE}",
  "keycloak_base": "${KEYCLOAK_BASE}",
  "agentd_port": ${AGENTD_PORT},
  "broker_port": ${BROKER_PORT},
  "webui_port": ${WEBUI_PORT},
  "postgres_port": ${POSTGRES_PORT},
  "keycloak_port": ${KEYCLOAK_PORT},
  "agentd_pid": ${AGENTD_PID},
  "broker_pid": ${BROKER_PID},
  "connector_pid": ${CONNECTOR_PID},
  "webui_pid": ${WEBUI_PID:-0},
  "compose_project": "${COMPOSE_PROJECT_NAME}",
  "compose_file": "${COMPOSE_FILE}",
  "agent_id": "${BROKER_AGENT_ID}",
  "logs_dir": "${LOG_DIR}",
}, indent=2))
PY
>"${STATE_PATH}"
ln -sf "${STATE_PATH}" "${ROOT}/out/devstack_state.json"

if [[ "${KEEP}" -eq 1 ]]; then
  CLEAN_ON_EXIT=0
  echo "[devstack] OK (services running)"
  echo "  - State:   ${STATE_PATH}"
  echo "  - WebUI:   ${WEBUI_BASE}"
  echo "  - agentd:  ${AGENTD_BASE}"
  echo "  - broker:  ${BROKER_BASE}"
  echo "  - down:    tools/devstack_agent_down.sh --state ${STATE_PATH}"
  exit 0
fi

echo "[devstack] OK (cleaning up)"
exit 0
