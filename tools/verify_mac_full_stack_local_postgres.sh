#!/usr/bin/env bash
set -euo pipefail

# macOS full-stack verification using local Postgres (no Docker/Keycloak).
# - Runs Postgres locally
# - Runs agentd + broker + connector + WebUI on the host
# - Uses broker client-auth (no OIDC); seeds the broker DB directly
#
# Optional env:
# - HOST_STACK_SKIP_UI=1 (skip WebUI build/serve)
# - HOST_STACK_UI_INSTALL=0 (skip npm ci when deps already exist)
# - HOST_STACK_PG_DSN=postgres://user@127.0.0.1:5432/agentd_broker?sslmode=disable
# - HOST_STACK_AGENT_ID=agent1
# - HOST_STACK_CLIENT_ID=dev-client
# - HOST_STACK_CLIENT_TOKEN=dev-broker-token
# - HOST_STACK_CLIENT_AUTH_FILE=/path/to/client_auth.json

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/lib/python_helpers.sh
source "${ROOT}/tools/lib/python_helpers.sh"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${ROOT}/tests/lib/agentd_smoke_lib.sh"
curl() {
  agentd_smoke_curl "$@"
}

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "[host-stack-local] WARNING: not running on macOS; continuing anyway"
fi

LOG_DIR="${ROOT}/out"
mkdir -p "${LOG_DIR}"
ts="$(date +%Y-%m-%d_%H%M%S)"
LOG_CMAKE_CONFIG="${LOG_DIR}/host_stack_local_cmake_config_${ts}.log"
LOG_CMAKE_BUILD="${LOG_DIR}/host_stack_local_cmake_build_${ts}.log"
LOG_AGENTD="${LOG_DIR}/host_stack_local_agentd_${ts}.log"
LOG_BROKER_BUILD="${LOG_DIR}/host_stack_local_broker_build_${ts}.log"
LOG_CONNECTOR_BUILD="${LOG_DIR}/host_stack_local_connector_build_${ts}.log"
LOG_BROKER="${LOG_DIR}/host_stack_local_broker_${ts}.log"
LOG_CONNECTOR="${LOG_DIR}/host_stack_local_connector_${ts}.log"
LOG_WEBUI_INSTALL="${LOG_DIR}/host_stack_local_webui_install_${ts}.log"
LOG_WEBUI_BUILD="${LOG_DIR}/host_stack_local_webui_build_${ts}.log"
LOG_WEBUI="${LOG_DIR}/host_stack_local_webui_${ts}.log"
LOG_MTLS="${LOG_DIR}/host_stack_local_mtls_${ts}.log"
LOG_DB="${LOG_DIR}/host_stack_local_db_${ts}.log"

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
  echo "[host-stack-local] ${label}"
  if ! "$@" >"${log}" 2>&1; then
    echo "[host-stack-local] FAILED: ${label} (log: ${log})" >&2
    tail -n 120 "${log}" >&2 || true
    return 1
  fi
  echo "[host-stack-local] OK: ${label} (log: ${log})"
}

wait_http_ok() {
  local url="$1"
  local timeout_s="${2:-240}"
  local started
  started="$(date +%s)"
  while true; do
    if curl -fsS -k "${url}" >/dev/null 2>&1; then
      return 0
    fi
    local now
    now="$(date +%s)"
    if (( now - started > timeout_s )); then
      echo "[host-stack-local] ERROR: timeout waiting for ${url}" >&2
      return 1
    fi
    sleep 1
  done
}

AGENTD_BIN="${AGENTD_BIN:-${ROOT}/build/agentd}"
AGENTD_PORT="${AGENTD_PORT:-$(pick_port)}"
AGENTD_AUTH_TOKEN="${AGENTD_AUTH_TOKEN:-dev-agentd-token}"
AGENTD_STATE_DIR="${AGENTD_STATE_DIR:-${ROOT}/state/host_stack_local}"

BROKER_BIN="${BROKER_BIN:-${ROOT}/out/host_stack/bin/agentd-broker}"
CONNECTOR_BIN="${CONNECTOR_BIN:-${ROOT}/out/host_stack/bin/agentd-connector}"
BROKER_PORT="${BROKER_PORT:-$(pick_port)}"

WEBUI_PORT="${WEBUI_PORT:-$(pick_port)}"
HOST_STACK_SKIP_UI="${HOST_STACK_SKIP_UI:-0}"
HOST_STACK_UI_INSTALL="${HOST_STACK_UI_INSTALL:-1}"

HOST_STACK_AGENT_ID="${HOST_STACK_AGENT_ID:-agent1}"
HOST_STACK_CLIENT_ID="${HOST_STACK_CLIENT_ID:-dev-client}"
HOST_STACK_CLIENT_TOKEN="${HOST_STACK_CLIENT_TOKEN:-}"
HOST_STACK_CLIENT_AUTH_FILE="${HOST_STACK_CLIENT_AUTH_FILE:-${ROOT}/out/host_stack_local/client_auth.json}"

HOST_STACK_PG_DSN="${HOST_STACK_PG_DSN:-}"
if [[ -z "${HOST_STACK_PG_DSN}" ]]; then
  HOST_STACK_PG_DSN="postgres://$(id -un)@127.0.0.1:5432/agentd_broker?sslmode=disable"
fi

eval "$(HOST_STACK_PG_DSN="${HOST_STACK_PG_DSN}" python3 - <<'PY'
import os
import shlex
import urllib.parse

dsn = os.environ.get("HOST_STACK_PG_DSN", "")
u = urllib.parse.urlparse(dsn)
scheme = u.scheme or "postgres"
host = u.hostname or "127.0.0.1"
port = u.port or 5432
user = u.username or ""
password = u.password or ""
db = (u.path or "").lstrip("/") or "agentd_broker"
query = u.query or ""

def build(dbname: str) -> str:
    auth = ""
    if user:
        auth = user
        if password:
            auth += ":" + password
        auth += "@"
    netloc = auth + host
    if port:
        netloc += f":{port}"
    out = f"{scheme}://{netloc}/{dbname}"
    if query:
        out += "?" + query
    return out

base = build("postgres")
out = {
    "PG_BASE_DSN": base,
    "PG_DB_NAME": db,
    "PG_HOST": host,
    "PG_PORT": str(port),
}
for k, v in out.items():
    print(f"{k}={shlex.quote(str(v))}")
PY
)"

MTLS_DIR="${ROOT}/out/host_stack_local/mtls"
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
}
trap cleanup EXIT

if ! command -v pg_isready >/dev/null 2>&1; then
  echo "[host-stack-local] pg_isready not found (install Postgres or use Docker stack)" >&2
  exit 77
fi
if ! command -v psql >/dev/null 2>&1; then
  echo "[host-stack-local] psql not found (install Postgres or use Docker stack)" >&2
  exit 77
fi

if ! pg_isready -h "${PG_HOST}" -p "${PG_PORT}" >/dev/null 2>&1; then
  echo "[host-stack-local] Postgres not ready at ${PG_HOST}:${PG_PORT}" >&2
  exit 77
fi

if ! command -v cmake >/dev/null 2>&1; then
  echo "[host-stack-local] cmake not found" >&2
  exit 1
fi
if ! command -v go >/dev/null 2>&1; then
  echo "[host-stack-local] go not found" >&2
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
    echo "[host-stack-local] python not found; skipping WebUI build/serve" >&2
    HOST_STACK_SKIP_UI=1
  fi
fi
if [[ "${HOST_STACK_SKIP_UI}" == "0" ]]; then
  if ! command -v npm >/dev/null 2>&1; then
    echo "[host-stack-local] npm not found; skipping WebUI" >&2
    HOST_STACK_SKIP_UI=1
  fi
fi

if [[ "${HOST_STACK_SKIP_UI}" == "0" ]]; then
  if [[ "${HOST_STACK_UI_INSTALL}" == "1" ]]; then
    run_logged "ui: npm ci" "${LOG_WEBUI_INSTALL}" bash -lc "cd ${ROOT}/ui && NPM_CONFIG_CACHE=./.npm-cache npm ci"
  fi
  if [[ ! -d "${ROOT}/ui/node_modules" ]]; then
    echo "[host-stack-local] UI deps missing; skipping WebUI" >&2
    HOST_STACK_SKIP_UI=1
  else
    run_logged "ui: npm run build" "${LOG_WEBUI_BUILD}" bash -lc "cd ${ROOT}/ui && NPM_CONFIG_CACHE=./.npm-cache npm run build"
  fi
fi

run_logged "generate mTLS certs" "${LOG_MTLS}" bash -lc "bash ${ROOT}/tools/gen_agentd_broker_mtls_test_certs.sh ${MTLS_DIR} ${HOST_STACK_AGENT_ID}"

echo "[host-stack-local] ensuring broker database exists"
if ! psql "${PG_BASE_DSN}" -tAc "SELECT 1 FROM pg_database WHERE datname='${PG_DB_NAME}'" >/dev/null 2>&1; then
  if ! psql "${PG_BASE_DSN}" -tAc "SELECT 1 FROM pg_database WHERE datname='${PG_DB_NAME}'" | grep -q 1; then
    if ! psql "${PG_BASE_DSN}" -v ON_ERROR_STOP=1 -c "CREATE DATABASE \"${PG_DB_NAME}\";" >"${LOG_DB}" 2>&1; then
      echo "[host-stack-local] ERROR: failed to create database ${PG_DB_NAME} (log: ${LOG_DB})" >&2
      tail -n 120 "${LOG_DB}" >&2 || true
      exit 1
    fi
  fi
fi

if [[ -z "${HOST_STACK_CLIENT_TOKEN}" ]]; then
  HOST_STACK_CLIENT_TOKEN="$(python3 - <<'PY'
import secrets
print(secrets.token_urlsafe(24))
PY
)"
fi

mkdir -p "$(dirname "${HOST_STACK_CLIENT_AUTH_FILE}")"
cat <<JSON >"${HOST_STACK_CLIENT_AUTH_FILE}"
{
  "clients": [
    {
      "client_id": "${HOST_STACK_CLIENT_ID}",
      "token": "${HOST_STACK_CLIENT_TOKEN}",
      "admin": true,
      "allowed_agents": ["${HOST_STACK_AGENT_ID}"]
    }
  ]
}
JSON

KEYCLOAK_BASE=""
AGENTD_BASE="http://127.0.0.1:${AGENTD_PORT}"
BROKER_BASE="https://127.0.0.1:${BROKER_PORT}"
WEBUI_BASE="http://127.0.0.1:${WEBUI_PORT}"
BROKER_DB_DSN="${HOST_STACK_PG_DSN}"

mkdir -p "${AGENTD_STATE_DIR}"
echo "[host-stack-local] starting agentd on 127.0.0.1:${AGENTD_PORT}"
"${AGENTD_BIN}" \
  --host 127.0.0.1 \
  --port "${AGENTD_PORT}" \
  --auth-token "${AGENTD_AUTH_TOKEN}" \
  --tools host \
  --state-dir "${AGENTD_STATE_DIR}" \
  --db-path "${AGENTD_STATE_DIR}/agentd.db" \
  --cors-origin "http://127.0.0.1:${WEBUI_PORT}" \
  --cors-origin "http://localhost:${WEBUI_PORT}" >"${LOG_AGENTD}" 2>&1 &
AGENTD_PID=$!

wait_http_ok "${AGENTD_BASE}/api/v1/health" 240 || true

echo "[host-stack-local] starting broker on 127.0.0.1:${BROKER_PORT}"
"${BROKER_BIN}" \
  --listen "127.0.0.1:${BROKER_PORT}" \
  --tls-cert "${MTLS_DIR}/server.pem" \
  --tls-key "${MTLS_DIR}/server.key.pem" \
  --tls-client-ca "${MTLS_DIR}/ca.pem" \
  --db-dsn "${BROKER_DB_DSN}" \
  --client-auth-file "${HOST_STACK_CLIENT_AUTH_FILE}" \
  --cors-origins "http://127.0.0.1:${WEBUI_PORT},http://localhost:${WEBUI_PORT}" >"${LOG_BROKER}" 2>&1 &
BROKER_PID=$!

wait_http_ok "${BROKER_BASE}/readyz" 240 || true

client_sub="client:${HOST_STACK_CLIENT_ID}"
psql "${BROKER_DB_DSN}" -v ON_ERROR_STOP=1 >"${LOG_DB}" 2>&1 <<SQL
INSERT INTO broker_users(sub) VALUES ('${client_sub}') ON CONFLICT DO NOTHING;
INSERT INTO broker_agents(agent_id, owner_sub, enabled, labels, meta)
VALUES ('${HOST_STACK_AGENT_ID}', '${client_sub}', true, '{}'::jsonb, '{}'::jsonb)
ON CONFLICT DO NOTHING;
INSERT INTO broker_agent_memberships(agent_id, user_sub, role)
VALUES ('${HOST_STACK_AGENT_ID}', '${client_sub}', 'owner')
ON CONFLICT DO NOTHING;
SQL

echo "[host-stack-local] starting connector for agent=${HOST_STACK_AGENT_ID}"
"${CONNECTOR_BIN}" \
  --broker "wss://127.0.0.1:${BROKER_PORT}/v1/agent/connect" \
  --local-agentd "${AGENTD_BASE}" \
  --tls-ca "${MTLS_DIR}/ca.pem" \
  --tls-cert "${MTLS_DIR}/client_${HOST_STACK_AGENT_ID}.pem" \
  --tls-key "${MTLS_DIR}/client_${HOST_STACK_AGENT_ID}.key.pem" \
  --agent-cn-prefix "agentd-" \
  --agent-id "${HOST_STACK_AGENT_ID}" >"${LOG_CONNECTOR}" 2>&1 &
CONNECTOR_PID=$!

if [[ "${HOST_STACK_SKIP_UI}" == "0" ]]; then
  cat <<CFG >"${ROOT}/ui/dist/agentui-config.js"
window.__AGENT_UI_CONFIG__ = {
  connectionMode: "broker",
  brokerBaseUrl: "${BROKER_BASE}",
  brokerAgentId: "${HOST_STACK_AGENT_ID}",
  brokerAuthToken: "${HOST_STACK_CLIENT_TOKEN}",
  daemonAuthToken: "${AGENTD_AUTH_TOKEN}",
};
CFG
  if http_server_cmd="$(python_http_server_cmd "${WEBUI_PORT}")"; then
    (cd "${ROOT}/ui/dist" && ${http_server_cmd} >"${LOG_WEBUI}" 2>&1) &
    WEBUI_PID=$!
  else
    echo "[host-stack-local] python not found; skipping WebUI serve" >&2
    HOST_STACK_SKIP_UI=1
  fi
fi

echo "[host-stack-local] waiting for broker proxy to reach agentd..."
started="$(date +%s)"
while true; do
  if curl -fsS -k \
    -H "Authorization: Bearer ${HOST_STACK_CLIENT_TOKEN}" \
    "${BROKER_BASE}/v1/agents/${HOST_STACK_AGENT_ID}/proxy/api/v1/health" >/dev/null 2>&1; then
    break
  fi
  now="$(date +%s)"
  if (( now - started > 240 )); then
    echo "[host-stack-local] ERROR: broker proxy did not reach agentd in time" >&2
    exit 4
  fi
  sleep 1
done

echo "[host-stack-local] OK"
echo "  - agentd: ${AGENTD_BASE}/api/v1/health (auth: ${AGENTD_AUTH_TOKEN})"
echo "  - broker: ${BROKER_BASE}/readyz (auth: Bearer ${HOST_STACK_CLIENT_TOKEN})"
echo "  - broker proxy: ${BROKER_BASE}/v1/agents/${HOST_STACK_AGENT_ID}/proxy/api/v1/health"
if [[ "${HOST_STACK_SKIP_UI}" == "0" ]]; then
  echo "  - webui: ${WEBUI_BASE}"
fi
