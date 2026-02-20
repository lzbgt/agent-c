#!/usr/bin/env bash
set -euo pipefail

# macOS full-stack verification without building Docker images.
# - Runs Postgres + Keycloak via docker compose (no-build)
# - Runs agentd + broker + connector + WebUI on the host
#
# Optional env:
# - HOST_STACK_SKIP_UI=1 (skip WebUI build/serve)
# - HOST_STACK_UI_INSTALL=0 (skip npm ci when deps already exist)

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${ROOT}/tests/lib/agentd_smoke_lib.sh"
curl() {
  agentd_smoke_curl "$@"
}

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "[host-stack] WARNING: not running on macOS; continuing anyway"
fi

LOG_DIR="${ROOT}/out"
mkdir -p "${LOG_DIR}"
ts="$(date +%Y-%m-%d_%H%M%S)"
LOG_CMAKE_CONFIG="${LOG_DIR}/host_stack_cmake_config_${ts}.log"
LOG_CMAKE_BUILD="${LOG_DIR}/host_stack_cmake_build_${ts}.log"
LOG_AGENTD="${LOG_DIR}/host_stack_agentd_${ts}.log"
LOG_BROKER_BUILD="${LOG_DIR}/host_stack_broker_build_${ts}.log"
LOG_CONNECTOR_BUILD="${LOG_DIR}/host_stack_connector_build_${ts}.log"
LOG_BROKER="${LOG_DIR}/host_stack_broker_${ts}.log"
LOG_CONNECTOR="${LOG_DIR}/host_stack_connector_${ts}.log"
LOG_WEBUI_INSTALL="${LOG_DIR}/host_stack_webui_install_${ts}.log"
LOG_WEBUI_BUILD="${LOG_DIR}/host_stack_webui_build_${ts}.log"
LOG_WEBUI="${LOG_DIR}/host_stack_webui_${ts}.log"
LOG_MTLS="${LOG_DIR}/host_stack_mtls_${ts}.log"
LOG_COMPOSE="${LOG_DIR}/host_stack_compose_${ts}.log"

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
  local label="${1}"
  local log="${2}"
  shift 2
  echo "[host-stack] ${label}"
  if ! "$@" >"${log}" 2>&1; then
    echo "[host-stack] FAILED: ${label} (log: ${log})" >&2
    tail -n 120 "${log}" >&2 || true
    return 1
  fi
  echo "[host-stack] OK: ${label} (log: ${log})"
}

AGENTD_BIN="${AGENTD_BIN:-${ROOT}/build/agentd}"
AGENTD_PORT="${AGENTD_PORT:-$(pick_port)}"
AGENTD_AUTH_TOKEN="${AGENTD_AUTH_TOKEN:-dev-agentd-token}"
AGENTD_STATE_DIR="${AGENTD_STATE_DIR:-${ROOT}/state/host_stack}"

BROKER_BIN="${BROKER_BIN:-${ROOT}/out/host_stack/bin/agentd-broker}"
CONNECTOR_BIN="${CONNECTOR_BIN:-${ROOT}/out/host_stack/bin/agentd-connector}"
BROKER_PORT="${BROKER_PORT:-$(pick_port)}"

POSTGRES_PORT="${POSTGRES_PUBLISHED_PORT:-$(pick_port)}"
KEYCLOAK_PORT="${KEYCLOAK_PUBLISHED_PORT:-$(pick_port)}"

WEBUI_PORT="${WEBUI_PORT:-$(pick_port)}"
HOST_STACK_SKIP_UI="${HOST_STACK_SKIP_UI:-0}"
HOST_STACK_UI_INSTALL="${HOST_STACK_UI_INSTALL:-1}"

COMPOSE_PROJECT_NAME="${COMPOSE_PROJECT_NAME:-agent_host_${WEBUI_PORT}}"
COMPOSE_FILE="${COMPOSE_FILE:-${ROOT}/docker-compose.yml}"
HOST_STACK_CLEAN="${HOST_STACK_CLEAN:-1}"

MTLS_DIR="${ROOT}/out/host_stack/mtls"
BIN_DIR="${ROOT}/out/host_stack/bin"
mkdir -p "${MTLS_DIR}" "${BIN_DIR}"

cleanup() {
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
  if [[ "${HOST_STACK_CLEAN}" == "1" ]]; then
    if docker_compose_preflight "host-stack-clean" >/dev/null 2>&1; then
      POSTGRES_PUBLISHED_PORT="${POSTGRES_PORT}" \
      KEYCLOAK_PUBLISHED_PORT="${KEYCLOAK_PORT}" \
      COMPOSE_PROJECT_NAME="${COMPOSE_PROJECT_NAME}" \
      docker compose -f "${COMPOSE_FILE}" down -v --remove-orphans >/dev/null 2>&1 || true
    fi
  fi
}
trap cleanup EXIT

source "${ROOT}/tools/lib/docker_preflight.sh"
# shellcheck source=tools/lib/python_helpers.sh
source "${ROOT}/tools/lib/python_helpers.sh"
if ! docker_compose_preflight "host-stack"; then
  exit 77
fi

if ! command -v cmake >/dev/null 2>&1; then
  echo "[host-stack] cmake not found" >&2
  exit 1
fi

if ! command -v go >/dev/null 2>&1; then
  echo "[host-stack] go not found" >&2
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

if [[ "${HOST_STACK_SKIP_UI}" == "0" ]]; then
  if ! command -v python3 >/dev/null 2>&1 && ! command -v python >/dev/null 2>&1; then
    echo "[host-stack] python not found; skipping WebUI build/serve" >&2
    HOST_STACK_SKIP_UI=1
  fi
fi

if [[ "${HOST_STACK_SKIP_UI}" == "0" ]]; then
  if ! command -v npm >/dev/null 2>&1; then
    echo "[host-stack] npm not found; skipping WebUI" >&2
    HOST_STACK_SKIP_UI=1
  fi
fi

if [[ "${HOST_STACK_SKIP_UI}" == "0" ]]; then
  if [[ "${HOST_STACK_UI_INSTALL}" == "1" ]]; then
    run_logged "ui: npm ci" "${LOG_WEBUI_INSTALL}" bash -lc "cd ${ROOT}/ui && NPM_CONFIG_CACHE=./.npm-cache npm ci"
  fi
  if [[ ! -d "${ROOT}/ui/node_modules" ]]; then
    echo "[host-stack] UI deps missing; skipping WebUI" >&2
    HOST_STACK_SKIP_UI=1
  else
    run_logged "ui: npm run build" "${LOG_WEBUI_BUILD}" bash -lc "cd ${ROOT}/ui && NPM_CONFIG_CACHE=./.npm-cache npm run build"
  fi
fi

run_logged "generate mTLS certs" "${LOG_MTLS}" bash -lc "bash ${ROOT}/tools/gen_agentd_broker_mtls_test_certs.sh ${MTLS_DIR} agent1"

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
    echo "[host-stack] SKIP: docker compose failed due to resource limits (runc/containerd). Try restarting Docker Desktop or increasing CPU/RAM." >&2
    exit 77
  fi
  echo "[host-stack] ERROR: docker compose failed (log: ${LOG_COMPOSE})" >&2
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
      echo "[host-stack] ERROR: timeout waiting for ${url}" >&2
      return 1
    fi
    sleep 1
  done
}

echo "[host-stack] waiting for keycloak OIDC discovery..."
wait_http_ok "${KEYCLOAK_BASE}/realms/agentd/.well-known/openid-configuration" 240

mkdir -p "${AGENTD_STATE_DIR}"
echo "[host-stack] starting agentd on 127.0.0.1:${AGENTD_PORT}"
"${AGENTD_BIN}" \
  --host 127.0.0.1 \
  --port "${AGENTD_PORT}" \
  --auth-token "${AGENTD_AUTH_TOKEN}" \
  --tools none \
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
  --tls-cert "${MTLS_DIR}/client_agent1.pem" \
  --tls-key "${MTLS_DIR}/client_agent1.key.pem" \
  --agent-cn-prefix "agentd-" \
  --agent-id "agent1" >"${LOG_CONNECTOR}" 2>&1 &
CONNECTOR_PID=$!

if [[ "${HOST_STACK_SKIP_UI}" == "0" ]]; then
  cat <<CFG >"${ROOT}/ui/dist/agentui-config.js"
window.__AGENT_UI_CONFIG__ = {
  connectionMode: "broker",
  brokerBaseUrl: "${BROKER_BASE}",
  brokerAgentId: "agent1",
  brokerAuthToken: "",
  daemonAuthToken: "${AGENTD_AUTH_TOKEN}",
};
CFG
  if http_server_cmd="$(python_http_server_cmd "${WEBUI_PORT}")"; then
    (cd "${ROOT}/ui/dist" && ${http_server_cmd} >"${LOG_WEBUI}" 2>&1) &
    WEBUI_PID=$!
  else
    echo "[host-stack] python not found; skipping WebUI serve" >&2
    HOST_STACK_SKIP_UI=1
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

echo "[host-stack] acquiring OIDC token (dev password grant)..."
OIDC_JWT="$(get_token)"
if [[ -z "${OIDC_JWT}" ]]; then
  echo "[host-stack] ERROR: failed to fetch OIDC token" >&2
  exit 2
fi

echo "[host-stack] creating broker agent record agent_id=agent1 (wait/retry)..."
started="$(date +%s)"
while true; do
  if curl -fsS -k \
    -H "Authorization: Bearer ${OIDC_JWT}" \
    -H "Content-Type: application/json" \
    -d '{"agent_id":"agent1"}' \
    "${BROKER_BASE}/v1/agents" >/dev/null 2>&1; then
    break
  fi
  now="$(date +%s)"
  if (( now - started > 240 )); then
    echo "[host-stack] ERROR: broker did not accept create agent in time" >&2
    exit 4
  fi
  sleep 1
 done

echo "[host-stack] waiting for connector to connect (agent1 connected=true)..."
started="$(date +%s)"
while true; do
  j="$(curl -fsS -k -H "Authorization: Bearer ${OIDC_JWT}" "${BROKER_BASE}/v1/agents" || true)"
  ok="$(python3 - "${j}" <<'PY'
import json,sys
raw = sys.argv[1] if len(sys.argv) > 1 else ""
try:
  obj = json.loads(raw or "{}")
  for a in (obj.get("agents") or []):
    if a.get("agent_id") == "agent1" and a.get("connected") is True:
      print("yes")
      raise SystemExit(0)
except Exception:
  pass
print("no")
PY
)"
  if [[ "${ok}" == "yes" ]]; then
    break
  fi
  now="$(date +%s)"
  if (( now - started > 240 )); then
    echo "[host-stack] ERROR: connector did not become connected in time" >&2
    echo "[host-stack] broker /v1/agents response:" >&2
    echo "${j}" >&2
    exit 3
  fi
  sleep 1
 done

echo "[host-stack] verifying broker proxy to agentd /api/v1/health..."
curl -fsS -k -H "Authorization: Bearer ${OIDC_JWT}" \
  "${BROKER_BASE}/v1/agents/agent1/proxy/api/v1/health" | python3 -m json.tool >/dev/null

echo "[host-stack] verifying direct agentd health (auth enabled)..."
curl -fsS -H "Authorization: Bearer ${AGENTD_AUTH_TOKEN}" \
  "${AGENTD_BASE}/api/v1/health" | python3 -m json.tool >/dev/null

if [[ "${HOST_STACK_SKIP_UI}" == "0" ]]; then
  echo "[host-stack] verifying webui is served..."
  curl -fsS "${WEBUI_BASE}/" >/dev/null
fi

echo "[host-stack] OK"
echo "  - WebUI:    ${WEBUI_BASE} (set Daemon auth token: ${AGENTD_AUTH_TOKEN})"
echo "  - agentd:   ${AGENTD_BASE}"
echo "  - Keycloak: ${KEYCLOAK_BASE} (realm=agentd user=test pass=test)"
echo "  - Broker:   ${BROKER_BASE} (OIDC aud=agentd-broker-dev agent_id=agent1)"
