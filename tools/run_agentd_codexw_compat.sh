#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<'EOF'
Usage: tools/run_agentd_codexw_compat.sh -u <broker-user> -p <broker-password> [options]

Starts local agentd and exposes it to codexw broker/iOS clients. The default
mode uses the proven codexw external-local-API facade. Native mode uses the
agentd deployment-connect connector directly.

Options:
  -u, --user <name>              Broker username.
  -p, --password <password>      Broker password.
  --broker-url <url>             Broker URL for codexw (default: codexw default, usually https://broker.hubstack.cn).
  --broker-mode <mode>           facade or native (default: facade).
  --deployment-id <id>           codexw deployment id (default: agentd-<hostname>).
  --agentd-bin <path>            agentd binary (default: build/agentd).
  --codexw-bin <path>            codexw binary (default: codexw from PATH).
  --agentd-port <port>           Local agentd port (default: 8123).
  --agentd-auth-token <token>    Local agentd auth token (default: generated process-local token).
  --facade-port <port>           Local codexw-compatible facade port (default: 8124).
  --facade-token <token>         Facade bearer token for codexw local API calls (default: generated).
  --facade-bin <path>            Facade executable (default: tools/agentd_codexw_local_api_facade.py).
  --turn-mode <mode>             Facade turn mode: agentd_run or echo (default: agentd_run).
  --native-connector-bin <path>  Native connector executable (default: tools/agentd_codexw_native_broker_connector.py).
  --native-identity-dir <path>   Native deployment identity dir (default: .codexw-agentd/native).
  --native-enrollment-token-id <id>
                                  One-time deployment enrollment token id for native first boot.
  --native-enrollment-secret <secret>
                                  One-time deployment enrollment token shared secret for native first boot.
                                  If omitted, native mode can mint a token with -u/-p.
  --native-no-reconnect          Disable native connector reconnect supervision.
  --state-dir <path>             agentd state dir (default: .codexw-agentd/state).
  --db-path <path>               agentd SQLite DB path (default: .codexw-agentd/agentd.sqlite).
  --no-build                     Do not run cmake build when build/agentd is missing.
  -h, --help                     Show this help.

Example:
  tools/run_agentd_codexw_compat.sh -u admin -p '<broker password>'
EOF
}

USER_NAME=""
PASSWORD=""
BROKER_URL=""
BROKER_MODE="facade"
DEPLOYMENT_ID=""
AGENTD_BIN="${ROOT}/build/agentd"
CODEXW_BIN="codexw"
AGENTD_PORT="8123"
AGENTD_AUTH_TOKEN=""
FACADE_PORT="8124"
FACADE_TOKEN=""
FACADE_BIN="${ROOT}/tools/agentd_codexw_local_api_facade.py"
TURN_MODE="agentd_run"
NATIVE_CONNECTOR_BIN="${ROOT}/tools/agentd_codexw_native_broker_connector.py"
NATIVE_IDENTITY_DIR="${ROOT}/.codexw-agentd/native"
NATIVE_ENROLLMENT_TOKEN_ID=""
NATIVE_ENROLLMENT_SECRET=""
NATIVE_RECONNECT=1
STATE_DIR="${ROOT}/.codexw-agentd/state"
DB_PATH="${ROOT}/.codexw-agentd/agentd.sqlite"
BUILD_IF_MISSING=1

while [[ $# -gt 0 ]]; do
  case "$1" in
    -u|--user) USER_NAME="${2:-}"; shift 2 ;;
    -p|--password) PASSWORD="${2:-}"; shift 2 ;;
    --broker-url) BROKER_URL="${2:-}"; shift 2 ;;
    --broker-mode) BROKER_MODE="${2:-}"; shift 2 ;;
    --deployment-id) DEPLOYMENT_ID="${2:-}"; shift 2 ;;
    --agentd-bin) AGENTD_BIN="${2:-}"; shift 2 ;;
    --codexw-bin) CODEXW_BIN="${2:-}"; shift 2 ;;
    --agentd-port) AGENTD_PORT="${2:-}"; shift 2 ;;
    --agentd-auth-token) AGENTD_AUTH_TOKEN="${2:-}"; shift 2 ;;
    --facade-port) FACADE_PORT="${2:-}"; shift 2 ;;
    --facade-token) FACADE_TOKEN="${2:-}"; shift 2 ;;
    --facade-bin) FACADE_BIN="${2:-}"; shift 2 ;;
    --turn-mode) TURN_MODE="${2:-}"; shift 2 ;;
    --native-connector-bin) NATIVE_CONNECTOR_BIN="${2:-}"; shift 2 ;;
    --native-identity-dir) NATIVE_IDENTITY_DIR="${2:-}"; shift 2 ;;
    --native-enrollment-token-id) NATIVE_ENROLLMENT_TOKEN_ID="${2:-}"; shift 2 ;;
    --native-enrollment-secret|--native-enrollment-shared-secret) NATIVE_ENROLLMENT_SECRET="${2:-}"; shift 2 ;;
    --native-no-reconnect) NATIVE_RECONNECT=0; shift ;;
    --state-dir) STATE_DIR="${2:-}"; shift 2 ;;
    --db-path) DB_PATH="${2:-}"; shift 2 ;;
    --no-build) BUILD_IF_MISSING=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

case "${BROKER_MODE}" in
  facade|native) ;;
  *) echo "--broker-mode must be facade or native" >&2; exit 2 ;;
esac

if [[ "${BROKER_MODE}" == "facade" && ( -z "${USER_NAME}" || -z "${PASSWORD}" ) ]]; then
  echo "broker --user and --password are required" >&2
  usage >&2
  exit 2
fi

if [[ "${BROKER_MODE}" == "native" && -z "${BROKER_URL}" ]]; then
  echo "--broker-url is required in --broker-mode native" >&2
  exit 2
fi

case "${TURN_MODE}" in
  agentd_run|echo) ;;
  *) echo "--turn-mode must be agentd_run or echo" >&2; exit 2 ;;
esac

if [[ -z "${DEPLOYMENT_ID}" ]]; then
  host="$(hostname -s 2>/dev/null || hostname 2>/dev/null || echo local)"
  DEPLOYMENT_ID="agentd-${host}"
fi

if [[ -z "${AGENTD_AUTH_TOKEN}" ]]; then
  AGENTD_AUTH_TOKEN="$(python3 - <<'PY'
import secrets
print("agentd-" + secrets.token_urlsafe(24))
PY
)"
fi

if [[ -z "${FACADE_TOKEN}" ]]; then
  FACADE_TOKEN="$(python3 - <<'PY'
import secrets
print("codexw-local-" + secrets.token_urlsafe(24))
PY
)"
fi

if [[ ! -x "${AGENTD_BIN}" ]]; then
  if [[ "${BUILD_IF_MISSING}" != "1" ]]; then
    echo "agentd binary not found or not executable: ${AGENTD_BIN}" >&2
    exit 1
  fi
  cmake -S "${ROOT}" -B "${ROOT}/build" >"${ROOT}/build/agentd_codexw_compat_cmake.log" 2>&1
  cmake --build "${ROOT}/build" --target agentd -j >"${ROOT}/build/agentd_codexw_compat_build.log" 2>&1
fi

if [[ "${BROKER_MODE}" == "facade" ]] && ! command -v "${CODEXW_BIN}" >/dev/null 2>&1 && [[ ! -x "${CODEXW_BIN}" ]]; then
  echo "codexw binary not found: ${CODEXW_BIN}" >&2
  echo "Install codexw or pass --codexw-bin /path/to/codexw." >&2
  exit 1
fi

if [[ "${BROKER_MODE}" == "facade" && ! -f "${FACADE_BIN}" ]]; then
  echo "facade script not found: ${FACADE_BIN}" >&2
  exit 1
fi

if [[ "${BROKER_MODE}" == "native" && ! -f "${NATIVE_CONNECTOR_BIN}" ]]; then
  echo "native connector script not found: ${NATIVE_CONNECTOR_BIN}" >&2
  exit 1
fi

mkdir -p "${STATE_DIR}" "$(dirname "${DB_PATH}")" "${NATIVE_IDENTITY_DIR}"

AGENTD_URL="http://127.0.0.1:${AGENTD_PORT}"
FACADE_URL="http://127.0.0.1:${FACADE_PORT}"
AGENTD_LOG="${ROOT}/build/agentd_codexw_compat_agentd.log"
FACADE_LOG="${ROOT}/build/agentd_codexw_compat_facade.log"
CODEXW_LOG="${ROOT}/build/agentd_codexw_compat_codexw.log"
NATIVE_LOG="${ROOT}/build/agentd_codexw_compat_native.log"

cleanup() {
  if [[ -n "${NATIVE_PID:-}" ]]; then
    kill -TERM "${NATIVE_PID}" >/dev/null 2>&1 || true
    wait "${NATIVE_PID}" >/dev/null 2>&1 || true
  fi
  if [[ -n "${FACADE_PID:-}" ]]; then
    kill -TERM "${FACADE_PID}" >/dev/null 2>&1 || true
    wait "${FACADE_PID}" >/dev/null 2>&1 || true
  fi
  if [[ -n "${AGENTD_PID:-}" ]]; then
    kill -TERM "${AGENTD_PID}" >/dev/null 2>&1 || true
    wait "${AGENTD_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

"${AGENTD_BIN}" \
  --host 127.0.0.1 \
  --port "${AGENTD_PORT}" \
  --auth-token "${AGENTD_AUTH_TOKEN}" \
  --tools host \
  --yolo \
  --state-dir "${STATE_DIR}" \
  --db-path "${DB_PATH}" \
  >"${AGENTD_LOG}" 2>&1 &
AGENTD_PID=$!

for _ in $(seq 1 80); do
  if curl -fsS --noproxy "*" "${AGENTD_URL}/api/v1/health" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

if ! curl -fsS --noproxy "*" "${AGENTD_URL}/api/v1/health" >/dev/null 2>&1; then
  echo "agentd did not become healthy; log: ${AGENTD_LOG}" >&2
  exit 1
fi

if [[ "${BROKER_MODE}" == "native" ]]; then
  native_args=(
    --broker-url "${BROKER_URL}"
    --deployment-id "${DEPLOYMENT_ID}"
    --display-name "${DEPLOYMENT_ID}"
    --identity-dir "${NATIVE_IDENTITY_DIR}"
    --agentd-base-url "${AGENTD_URL}"
    --agentd-auth-token "${AGENTD_AUTH_TOKEN}"
    --bootstrap-identity
    --connect
  )
  if [[ "${NATIVE_RECONNECT}" == "1" ]]; then
    native_args+=(--reconnect)
  fi
  if [[ -n "${USER_NAME}" ]]; then
    native_args+=(--broker-user "${USER_NAME}")
  fi
  if [[ -n "${PASSWORD}" ]]; then
    native_args+=(--broker-password "${PASSWORD}")
  fi
  if [[ -n "${NATIVE_ENROLLMENT_TOKEN_ID}" ]]; then
    native_args+=(--enrollment-token-id "${NATIVE_ENROLLMENT_TOKEN_ID}")
  fi
  if [[ -n "${NATIVE_ENROLLMENT_SECRET}" ]]; then
    native_args+=(--enrollment-shared-secret "${NATIVE_ENROLLMENT_SECRET}")
  fi

  echo "agentd: ${AGENTD_URL}"
  echo "agentd log: ${AGENTD_LOG}"
  echo "native codexw broker connector identity dir: ${NATIVE_IDENTITY_DIR}"
  echo "native codexw broker connector log: ${NATIVE_LOG}"
  echo "codexw deployment id: ${DEPLOYMENT_ID}"
  echo "AGENTD_BASE_URL=${AGENTD_URL}"
  AGENTD_BASE_URL="${AGENTD_URL}" \
  AGENTD_AUTH_TOKEN="${AGENTD_AUTH_TOKEN}" \
  "${NATIVE_CONNECTOR_BIN}" "${native_args[@]}" >"${NATIVE_LOG}" 2>&1 &
  NATIVE_PID=$!
  wait "${NATIVE_PID}"
  exit $?
fi

"${FACADE_BIN}" \
  --host 127.0.0.1 \
  --port "${FACADE_PORT}" \
  --token "${FACADE_TOKEN}" \
  --agentd-base-url "${AGENTD_URL}" \
  --agentd-auth-token "${AGENTD_AUTH_TOKEN}" \
  --agentd-tools host \
  --deployment-id "${DEPLOYMENT_ID}" \
  --file-root "${ROOT}" \
  --turn-mode "${TURN_MODE}" \
  --quiet \
  >"${FACADE_LOG}" 2>&1 &
FACADE_PID=$!

for _ in $(seq 1 80); do
  if curl -fsS --noproxy "*" -H "Authorization: Bearer ${FACADE_TOKEN}" "${FACADE_URL}/healthz" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

if ! curl -fsS --noproxy "*" -H "Authorization: Bearer ${FACADE_TOKEN}" "${FACADE_URL}/healthz" >/dev/null 2>&1; then
  echo "codexw local-api facade did not become healthy; log: ${FACADE_LOG}" >&2
  exit 1
fi

codexw_args=(
  deploy
  -u "${USER_NAME}"
  -p "${PASSWORD}"
  --cwd "${ROOT}"
  --id "${DEPLOYMENT_ID}"
  --deployment-local-api-base-url "${FACADE_URL}"
  --local-api-token "${FACADE_TOKEN}"
)
if [[ -n "${BROKER_URL}" ]]; then
  codexw_args+=(--broker-url "${BROKER_URL}")
fi

echo "agentd: ${AGENTD_URL}"
echo "agentd log: ${AGENTD_LOG}"
echo "codexw local-api facade: ${FACADE_URL}"
echo "codexw local-api facade log: ${FACADE_LOG}"
echo "codexw deployment id: ${DEPLOYMENT_ID}"
echo "codexw log: ${CODEXW_LOG}"
echo "AGENTD_BASE_URL=${AGENTD_URL}"
echo "CODEXW_LOCAL_API_BASE_URL=${FACADE_URL}"

AGENTD_BASE_URL="${AGENTD_URL}" \
AGENTD_AUTH_TOKEN="${AGENTD_AUTH_TOKEN}" \
CODEXW_LOCAL_API_BASE_URL="${FACADE_URL}" \
CODEXW_LOCAL_API_TOKEN="${FACADE_TOKEN}" \
"${CODEXW_BIN}" "${codexw_args[@]}" >"${CODEXW_LOG}" 2>&1
