#!/usr/bin/env bash
set -euo pipefail

# One-command dev stack (agentd + broker + connector + WebUI) on macOS host.
# - Runs Postgres + Keycloak via docker compose (no build)
# - Runs agentd + broker + connector + WebUI on the host
# - Performs smoke checks + captures evidence bundle

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${ROOT}/tests/lib/agentd_smoke_lib.sh"
curl() {
  agentd_smoke_curl "$@"
}

usage() {
  cat <<'USAGE'
Usage: tools/devstack_agent.sh [options]

Options:
  --keep           Keep services running after checks (default).
  --no-keep        Stop services after checks.
  --restart        Stop any live canonical devstack before starting a new one.
  --skip-ui        Skip WebUI build/serve.
  --ui-install     Run npm ci before building UI (default: 1).
  --agent-count    Number of agentd instances to start (default: 1).
  --workflow-http  Enable workflow http/agentd_call tasks on the primary agentd.
  --agentd-tools   agentd tools mode (default: host)
  --agentd-port    agentd port (default: random)
  --broker-port    broker port (default: random)
  --broker-tls     Enable TLS + mTLS for broker/connector (default: off)
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
# shellcheck source=tools/lib/devstack_state.sh
source "${ROOT}/tools/lib/devstack_state.sh"
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
RESTART=0
SKIP_UI=0
UI_INSTALL=1
AGENTD_TOOLS="host"
AGENT_COUNT=1
WORKFLOW_HTTP=0
AGENTD_PORT=""
BROKER_PORT=""
BROKER_TLS="${BROKER_TLS:-0}"
WEBUI_PORT=""
POSTGRES_PORT=""
KEYCLOAK_PORT=""
OUT_DIR=""
AGENTD_STATE_DIR=""
STATE_LINK="${ROOT}/out/devstack_state.json"
TOPOLOGY_OVERRIDES=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --keep) KEEP=1; shift ;;
    --no-keep) KEEP=0; shift ;;
    --restart) RESTART=1; shift ;;
    --skip-ui) SKIP_UI=1; shift ;;
    --ui-install) UI_INSTALL=1; shift ;;
    --agent-count) AGENT_COUNT="$2"; TOPOLOGY_OVERRIDES+=("$1"); shift 2 ;;
    --workflow-http) WORKFLOW_HTTP=1; TOPOLOGY_OVERRIDES+=("$1"); shift ;;
    --agentd-tools) AGENTD_TOOLS="$2"; TOPOLOGY_OVERRIDES+=("$1"); shift 2 ;;
    --agentd-port) AGENTD_PORT="$2"; TOPOLOGY_OVERRIDES+=("$1"); shift 2 ;;
    --broker-port) BROKER_PORT="$2"; TOPOLOGY_OVERRIDES+=("$1"); shift 2 ;;
    --broker-tls) BROKER_TLS=1; TOPOLOGY_OVERRIDES+=("$1"); shift ;;
    --webui-port) WEBUI_PORT="$2"; TOPOLOGY_OVERRIDES+=("$1"); shift 2 ;;
    --postgres-port) POSTGRES_PORT="$2"; TOPOLOGY_OVERRIDES+=("$1"); shift 2 ;;
    --keycloak-port) KEYCLOAK_PORT="$2"; TOPOLOGY_OVERRIDES+=("$1"); shift 2 ;;
    --out-dir) OUT_DIR="$2"; TOPOLOGY_OVERRIDES+=("$1"); shift 2 ;;
    --state-dir) AGENTD_STATE_DIR="$2"; TOPOLOGY_OVERRIDES+=("$1"); shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown arg: $1" >&2; usage; exit 1 ;;
  esac
done

AGENTD_BIN="${AGENTD_BIN:-${ROOT}/build/agentd}"
BROKER_BIN="${BROKER_BIN:-${ROOT}/out/devstack/bin/agentd-broker}"
CONNECTOR_BIN="${CONNECTOR_BIN:-${ROOT}/out/devstack/bin/agentd-connector}"
AGENTD_AUTH_TOKEN="${AGENTD_AUTH_TOKEN:-dev-agentd-token}"

if ! [[ "${AGENT_COUNT}" =~ ^[0-9]+$ ]]; then
  echo "[devstack] invalid --agent-count: ${AGENT_COUNT}" >&2
  exit 2
fi
if [[ "${AGENT_COUNT}" -lt 1 ]]; then
  echo "[devstack] agent-count must be >= 1" >&2
  exit 2
fi
if [[ "${BROKER_TLS}" != "0" && "${BROKER_TLS}" != "1" ]]; then
  echo "[devstack] invalid BROKER_TLS (expected 0 or 1): ${BROKER_TLS}" >&2
  exit 2
fi

if [[ -f "${STATE_LINK}" ]]; then
  if devstack_state_is_live "${STATE_LINK}"; then
    if [[ "${RESTART}" -eq 1 ]]; then
      echo "[devstack] restarting live canonical stack from ${STATE_LINK}"
      "${ROOT}/tools/devstack_agent_down.sh" --state "${STATE_LINK}"
    elif [[ "${#TOPOLOGY_OVERRIDES[@]}" -gt 0 ]]; then
      echo "[devstack] live canonical stack already running; refusing to start a second broker/WebUI" >&2
      echo "[devstack] requested topology flags: ${TOPOLOGY_OVERRIDES[*]}" >&2
      echo "[devstack] rerun with --restart or stop the current stack first:" >&2
      echo "  tools/devstack_agent_down.sh --state ${STATE_LINK}" >&2
      exit 2
    else
      echo "[devstack] reusing live canonical stack from ${STATE_LINK}"
      "${ROOT}/tools/devstack_status.sh" --state "${STATE_LINK}" || true
      exit 0
    fi
  else
    echo "[devstack] cleaning stale canonical state from ${STATE_LINK}"
    devstack_state_cleanup_stale "${ROOT}" "${STATE_LINK}"
  fi
fi

AGENT_IDS=()
for i in $(seq 1 "${AGENT_COUNT}"); do
  AGENT_IDS+=("agent${i}")
done

if [[ "${AGENT_COUNT}" -gt 1 && "${WORKFLOW_HTTP}" -eq 0 ]]; then
  WORKFLOW_HTTP=1
fi

BROKER_AGENT_ID="${BROKER_AGENT_ID:-${AGENT_IDS[0]}}"

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

AGENTD_LOGS=()
CONNECTOR_LOGS=()
AGENTD_PIDS=()
CONNECTOR_PIDS=()
AGENTD_BASES=()
AGENTD_PORTS=()
CLEANED_AGENT_IDS=()

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
  if declare -p CONNECTOR_PIDS >/dev/null 2>&1 && [[ "${#CONNECTOR_PIDS[@]}" -gt 0 ]]; then
    for pid in "${CONNECTOR_PIDS[@]}"; do
      if [[ -n "${pid}" ]]; then
        kill "${pid}" >/dev/null 2>&1 || true
      fi
    done
  elif [[ -n "${CONNECTOR_PID:-}" ]]; then
    kill "${CONNECTOR_PID}" >/dev/null 2>&1 || true
  fi
  if [[ -n "${BROKER_PID:-}" ]]; then
    kill "${BROKER_PID}" >/dev/null 2>&1 || true
  fi
  if declare -p AGENTD_PIDS >/dev/null 2>&1 && [[ "${#AGENTD_PIDS[@]}" -gt 0 ]]; then
    for pid in "${AGENTD_PIDS[@]}"; do
      if [[ -n "${pid}" ]]; then
        kill "${pid}" >/dev/null 2>&1 || true
      fi
    done
  elif [[ -n "${AGENTD_PID:-}" ]]; then
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
  if ! command -v python3 >/dev/null 2>&1 && ! command -v python >/dev/null 2>&1; then
    echo "[devstack] python not found; skipping WebUI build/serve" >&2
    SKIP_UI=1
  fi
fi

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

if [[ "${BROKER_TLS}" -eq 1 ]]; then
  run_logged "generate mTLS certs" "${LOG_MTLS}" bash -lc "bash ${ROOT}/tools/gen_agentd_broker_mtls_test_certs.sh ${MTLS_DIR} ${AGENT_IDS[*]}"
fi

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

mkdir -p "${AGENTD_STATE_DIR}"

for idx in "${!AGENT_IDS[@]}"; do
  agent_id="${AGENT_IDS[$idx]}"
  if [[ "${idx}" -eq 0 ]]; then
    port="${AGENTD_PORT}"
  else
    port="$(pick_port)"
  fi
  AGENTD_PORTS+=("${port}")
  AGENTD_BASES+=("http://127.0.0.1:${port}")
  agent_state="${AGENTD_STATE_DIR}/${agent_id}"
  mkdir -p "${agent_state}"
  log="${LOG_DIR}/agentd_${agent_id}.log"
  AGENTD_LOGS+=("${log}")
  if [[ "${idx}" -eq 0 && "${WORKFLOW_HTTP}" -eq 1 ]]; then
    AGENTD_CALL_BEARER="${AGENTD_AUTH_TOKEN}" nohup "${AGENTD_BIN}" \
      --host 127.0.0.1 \
      --port "${port}" \
      --auth-token "${AGENTD_AUTH_TOKEN}" \
      --tools "${AGENTD_TOOLS}" \
      --host-policy full \
      --yolo \
      --state-dir "${agent_state}" \
      --db-path "${agent_state}/agentd.db" \
      --cors-origin "http://127.0.0.1:${WEBUI_PORT}" \
      --cors-origin "http://localhost:${WEBUI_PORT}" \
      --workflow-enable-http-tasks \
      --workflow-http-allow-host 127.0.0.1 >"${log}" 2>&1 &
  else
    AGENTD_CALL_BEARER="${AGENTD_AUTH_TOKEN}" nohup "${AGENTD_BIN}" \
      --host 127.0.0.1 \
      --port "${port}" \
      --auth-token "${AGENTD_AUTH_TOKEN}" \
      --tools "${AGENTD_TOOLS}" \
      --host-policy full \
      --yolo \
      --state-dir "${agent_state}" \
      --db-path "${agent_state}/agentd.db" \
      --cors-origin "http://127.0.0.1:${WEBUI_PORT}" \
      --cors-origin "http://localhost:${WEBUI_PORT}" >"${log}" 2>&1 &
  fi
  AGENTD_PIDS+=("$!")
done

AGENTD_PORT="${AGENTD_PORTS[0]}"
AGENTD_BASE="${AGENTD_BASES[0]}"
LOG_AGENTD="${AGENTD_LOGS[0]}"
AGENTD_PID="${AGENTD_PIDS[0]}"

wait_http_ok "${AGENTD_BASE}/api/v1/health" 240 || true

BROKER_DB_DSN="postgres://postgres:postgres@127.0.0.1:${POSTGRES_PORT}/agentd_broker?sslmode=disable"
BROKER_TLS_ARGS=()

if [[ "${BROKER_TLS}" -eq 1 ]]; then
  BROKER_BASE="https://127.0.0.1:${BROKER_PORT}"
  BROKER_CONNECT_URL="wss://127.0.0.1:${BROKER_PORT}/v1/agent/connect"
  BROKER_TLS_ARGS=(--tls-cert "${MTLS_DIR}/server.pem" --tls-key "${MTLS_DIR}/server.key.pem" --tls-client-ca "${MTLS_DIR}/ca.pem")
else
  BROKER_BASE="http://127.0.0.1:${BROKER_PORT}"
  BROKER_CONNECT_URL="ws://127.0.0.1:${BROKER_PORT}/v1/agent/connect"
  BROKER_TLS_ARGS=(--require-agent-mtls=false)
fi

broker_curl() {
  if [[ "${BROKER_TLS}" -eq 1 ]]; then
    curl -fsS -k "$@"
  else
    curl -fsS "$@"
  fi
}

nohup "${BROKER_BIN}" \
  --listen "127.0.0.1:${BROKER_PORT}" \
  "${BROKER_TLS_ARGS[@]}" \
  --db-dsn "${BROKER_DB_DSN}" \
  --oidc-issuer "${KEYCLOAK_BASE}/realms/agentd" \
  --oidc-audience "agentd-broker-dev" \
  --cors-origins "http://127.0.0.1:${WEBUI_PORT},http://localhost:${WEBUI_PORT}" >"${LOG_BROKER}" 2>&1 &
BROKER_PID=$!

for idx in "${!AGENT_IDS[@]}"; do
  agent_id="${AGENT_IDS[$idx]}"
  base="${AGENTD_BASES[$idx]}"
  log="${LOG_DIR}/connector_${agent_id}.log"
  CONNECTOR_LOGS+=("${log}")
  if [[ "${BROKER_TLS}" -eq 1 ]]; then
    nohup "${CONNECTOR_BIN}" \
      --broker "${BROKER_CONNECT_URL}" \
      --local-agentd "${base}" \
      --tls-ca "${MTLS_DIR}/ca.pem" \
      --tls-cert "${MTLS_DIR}/client_${agent_id}.pem" \
      --tls-key "${MTLS_DIR}/client_${agent_id}.key.pem" \
      --agent-cn-prefix "agentd-" \
      --agent-id "${agent_id}" >"${log}" 2>&1 &
  else
    nohup "${CONNECTOR_BIN}" \
      --broker "${BROKER_CONNECT_URL}" \
      --local-agentd "${base}" \
      --agent-cn-prefix "agentd-" \
      --agent-id "${agent_id}" >"${log}" 2>&1 &
  fi
  CONNECTOR_PIDS+=("$!")
done
LOG_CONNECTOR="${CONNECTOR_LOGS[0]}"
CONNECTOR_PID="${CONNECTOR_PIDS[0]}"

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

workflow_targets_json="$(
  printf '%s\n' "${AGENTD_BASES[@]}" | python3 - <<'PY'
import json,sys
items = [line.strip() for line in sys.stdin.read().splitlines() if line.strip()]
print(json.dumps(items))
PY
)"

if [[ "${SKIP_UI}" -eq 0 ]]; then
  cat <<CFG >"${ROOT}/ui/dist/agentui-config.js"
window.__AGENT_UI_CONFIG__ = {
  connectionMode: "broker",
  brokerBaseUrl: "${BROKER_BASE}",
  brokerAgentId: "${BROKER_AGENT_ID}",
  brokerAuthToken: "${OIDC_JWT}",
  daemonAuthToken: "${AGENTD_AUTH_TOKEN}",
  brokerPanelOpen: true,
  workflowAgentTargets: ${workflow_targets_json},
  workflowBearerEnv: "AGENTD_CALL_BEARER",
};
CFG
  if http_server_cmd="$(python_http_server_cmd "${WEBUI_PORT}")"; then
    bash -lc "cd '${ROOT}/ui/dist' && exec ${http_server_cmd}" >"${LOG_WEBUI}" 2>&1 &
    WEBUI_PID=$!
  else
    echo "[devstack] python not found; skipping WebUI serve" >&2
    SKIP_UI=1
  fi
fi

started="$(date +%s)"
while true; do
  ok=1
  for agent_id in "${AGENT_IDS[@]}"; do
    if ! broker_curl \
      -H "Authorization: Bearer ${OIDC_JWT}" \
      -H "Content-Type: application/json" \
      -d "{\"agent_id\":\"${agent_id}\"}" \
      "${BROKER_BASE}/v1/agents" >/dev/null 2>&1; then
      # If creation fails, check whether the agent already exists for this user.
      list_json="$(
        broker_curl \
          -H "Authorization: Bearer ${OIDC_JWT}" \
          "${BROKER_BASE}/v1/agents" 2>/dev/null || true
      )"
      exists="$(
        AGENT_ID="${agent_id}" python3 - <<'PY'
import json,os,sys
agent_id = os.environ.get("AGENT_ID", "")
try:
  data=json.loads(sys.stdin.read() or "{}")
except Exception:
  data={}
agents=data.get("agents") or []
print("1" if any(isinstance(a,dict) and a.get("agent_id")==agent_id for a in agents) else "0")
PY
      <<<"${list_json}"
      )"
      if [[ "${exists}" != "1" ]]; then
        cleaned=0
        for seen in "${CLEANED_AGENT_IDS[@]-}"; do
          if [[ "${seen}" == "${agent_id}" ]]; then
            cleaned=1
            break
          fi
        done
        if [[ "${cleaned}" -eq 0 ]] && docker_compose_preflight "devstack-clean" >/dev/null 2>&1; then
          CLEANED_AGENT_IDS+=("${agent_id}")
          postgres_container="${COMPOSE_PROJECT_NAME}-postgres-1"
          docker exec -i "${postgres_container}" \
            psql -U postgres -d agentd_broker \
            -c "DELETE FROM broker_agents WHERE agent_id='${agent_id}';" >/dev/null 2>&1 || true
          ok=0
          break
        fi
        ok=0
        break
      fi
    fi
  done
  if [[ "${ok}" -eq 1 ]]; then
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
  j="$(broker_curl -H "Authorization: Bearer ${OIDC_JWT}" "${BROKER_BASE}/v1/agents" || true)"
  ok="$(python3 - "${j}" "${AGENT_IDS[@]}" <<'PY'
import json,sys
raw = sys.argv[1] if len(sys.argv) > 1 else ""
expected = set(sys.argv[2:])
connected = set()
try:
  obj = json.loads(raw or "{}")
  for a in (obj.get("agents") or []):
    if a.get("connected") is True and a.get("agent_id"):
      connected.add(a.get("agent_id"))
except Exception:
  pass
print("yes" if expected.issubset(connected) else "no")
PY
)"
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

broker_curl -H "Authorization: Bearer ${OIDC_JWT}" \
  "${BROKER_BASE}/v1/agents/${BROKER_AGENT_ID}/proxy/api/v1/health" >/dev/null || true

curl -fsS -H "Authorization: Bearer ${AGENTD_AUTH_TOKEN}" \
  "${AGENTD_BASE}/api/v1/health" >/dev/null || true

if [[ "${SKIP_UI}" -eq 0 ]]; then
  curl -fsS "${WEBUI_BASE}/" >/dev/null || true
fi

"${ROOT}/tools/capture_agent_evidence_bundle.sh" \
  --agentd-base "${AGENTD_BASE}" \
  --agentd-token "${AGENTD_AUTH_TOKEN}" \
  --broker-base "${BROKER_BASE}" \
  --broker-token "${OIDC_JWT}" \
  --broker-agent-id "${BROKER_AGENT_ID}" \
  --agentd-via-broker >"${LOG_EVIDENCE}" 2>&1 || true

STATE_PATH="${OUT_DIR}/devstack_state.json"
AGENT_IDS_CSV="$(IFS=,; echo "${AGENT_IDS[*]}")"
AGENTD_BASES_CSV="$(IFS=,; echo "${AGENTD_BASES[*]}")"
AGENTD_PORTS_CSV="$(IFS=,; echo "${AGENTD_PORTS[*]}")"
AGENTD_PIDS_CSV="$(IFS=,; echo "${AGENTD_PIDS[*]}")"
CONNECTOR_PIDS_CSV="$(IFS=,; echo "${CONNECTOR_PIDS[*]}")"

python3 - <<PY >"${STATE_PATH}"
import json

def split_list(raw):
  return [x for x in (raw or "").split(",") if x]

broker_tls = "${BROKER_TLS}" == "1"

agent_ids = split_list("${AGENT_IDS_CSV}")
agentd_bases = split_list("${AGENTD_BASES_CSV}")
agentd_ports = split_list("${AGENTD_PORTS_CSV}")
agentd_pids = split_list("${AGENTD_PIDS_CSV}")
connector_pids = split_list("${CONNECTOR_PIDS_CSV}")

agents = []
for idx, agent_id in enumerate(agent_ids):
  base = agentd_bases[idx] if idx < len(agentd_bases) else ""
  port = int(agentd_ports[idx]) if idx < len(agentd_ports) and agentd_ports[idx].isdigit() else 0
  pid = int(agentd_pids[idx]) if idx < len(agentd_pids) and agentd_pids[idx].isdigit() else 0
  cpid = int(connector_pids[idx]) if idx < len(connector_pids) and connector_pids[idx].isdigit() else 0
  agents.append({
    "agent_id": agent_id,
    "agentd_base": base,
    "agentd_port": port,
    "agentd_pid": pid,
    "connector_pid": cpid,
  })

print(json.dumps({
  "out_dir": "${OUT_DIR}",
  "agentd_base": "${AGENTD_BASE}",
  "broker_base": "${BROKER_BASE}",
  "broker_tls": broker_tls,
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
  "agents": agents,
  "logs_dir": "${LOG_DIR}",
}, indent=2))
PY
ln -sf "${STATE_PATH}" "${STATE_LINK}"

if [[ "${KEEP}" -eq 1 ]]; then
  CLEAN_ON_EXIT=0
  echo "[devstack] OK (services running)"
  echo "  - State:   ${STATE_PATH}"
  echo "  - WebUI:   ${WEBUI_BASE}"
  echo "  - agentd:  ${AGENTD_BASE}"
  echo "  - broker:  ${BROKER_BASE}"
  if [[ "${#AGENTD_BASES[@]}" -gt 1 ]]; then
    echo "  - agents:"
    for idx in "${!AGENT_IDS[@]}"; do
      echo "      - ${AGENT_IDS[$idx]} -> ${AGENTD_BASES[$idx]}"
    done
  fi
  echo "  - down:    tools/devstack_agent_down.sh --state ${STATE_PATH}"
  exit 0
fi

echo "[devstack] OK (cleaning up)"
exit 0
