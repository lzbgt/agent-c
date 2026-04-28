#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<'EOF'
Usage: tools/run_agentd_codexw_compat.sh -u <broker-user> -p <broker-password> [options]

Starts local agentd and then starts a sibling codexw broker deployment session
from this workspace so broker/iOS clients can reach the host through the
currently supported codexw deployment surface.

Options:
  -u, --user <name>              Broker username.
  -p, --password <password>      Broker password.
  --broker-url <url>             Broker URL for codexw (default: codexw default, usually https://broker.hubstack.cn).
  --deployment-id <id>           codexw deployment id (default: agentd-<hostname>).
  --agentd-bin <path>            agentd binary (default: build/agentd).
  --codexw-bin <path>            codexw binary (default: codexw from PATH).
  --agentd-port <port>           Local agentd port (default: 8123).
  --agentd-auth-token <token>    Local agentd auth token (default: generated process-local token).
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
DEPLOYMENT_ID=""
AGENTD_BIN="${ROOT}/build/agentd"
CODEXW_BIN="codexw"
AGENTD_PORT="8123"
AGENTD_AUTH_TOKEN=""
STATE_DIR="${ROOT}/.codexw-agentd/state"
DB_PATH="${ROOT}/.codexw-agentd/agentd.sqlite"
BUILD_IF_MISSING=1

while [[ $# -gt 0 ]]; do
  case "$1" in
    -u|--user) USER_NAME="${2:-}"; shift 2 ;;
    -p|--password) PASSWORD="${2:-}"; shift 2 ;;
    --broker-url) BROKER_URL="${2:-}"; shift 2 ;;
    --deployment-id) DEPLOYMENT_ID="${2:-}"; shift 2 ;;
    --agentd-bin) AGENTD_BIN="${2:-}"; shift 2 ;;
    --codexw-bin) CODEXW_BIN="${2:-}"; shift 2 ;;
    --agentd-port) AGENTD_PORT="${2:-}"; shift 2 ;;
    --agentd-auth-token) AGENTD_AUTH_TOKEN="${2:-}"; shift 2 ;;
    --state-dir) STATE_DIR="${2:-}"; shift 2 ;;
    --db-path) DB_PATH="${2:-}"; shift 2 ;;
    --no-build) BUILD_IF_MISSING=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ -z "${USER_NAME}" || -z "${PASSWORD}" ]]; then
  echo "broker --user and --password are required" >&2
  usage >&2
  exit 2
fi

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

if [[ ! -x "${AGENTD_BIN}" ]]; then
  if [[ "${BUILD_IF_MISSING}" != "1" ]]; then
    echo "agentd binary not found or not executable: ${AGENTD_BIN}" >&2
    exit 1
  fi
  cmake -S "${ROOT}" -B "${ROOT}/build" >"${ROOT}/build/agentd_codexw_compat_cmake.log" 2>&1
  cmake --build "${ROOT}/build" --target agentd -j >"${ROOT}/build/agentd_codexw_compat_build.log" 2>&1
fi

if ! command -v "${CODEXW_BIN}" >/dev/null 2>&1 && [[ ! -x "${CODEXW_BIN}" ]]; then
  echo "codexw binary not found: ${CODEXW_BIN}" >&2
  echo "Install codexw or pass --codexw-bin /path/to/codexw." >&2
  exit 1
fi

mkdir -p "${STATE_DIR}" "$(dirname "${DB_PATH}")"

AGENTD_URL="http://127.0.0.1:${AGENTD_PORT}"
AGENTD_LOG="${ROOT}/build/agentd_codexw_compat_agentd.log"
CODEXW_LOG="${ROOT}/build/agentd_codexw_compat_codexw.log"

cleanup() {
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
  --host-scope "${ROOT}" \
  --tools-root "@host" \
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

codexw_args=(deploy -u "${USER_NAME}" -p "${PASSWORD}" --cwd "${ROOT}" --id "${DEPLOYMENT_ID}")
if [[ -n "${BROKER_URL}" ]]; then
  codexw_args+=(--deployment-broker-url "${BROKER_URL}")
fi

echo "agentd: ${AGENTD_URL}"
echo "agentd log: ${AGENTD_LOG}"
echo "codexw deployment id: ${DEPLOYMENT_ID}"
echo "codexw log: ${CODEXW_LOG}"
echo "AGENTD_BASE_URL=${AGENTD_URL}"

AGENTD_BASE_URL="${AGENTD_URL}" \
AGENTD_AUTH_TOKEN="${AGENTD_AUTH_TOKEN}" \
"${CODEXW_BIN}" "${codexw_args[@]}" >"${CODEXW_LOG}" 2>&1
