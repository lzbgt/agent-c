#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "missing agentd binary path arg" >&2
  exit 2
fi

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

HOST="127.0.0.1"
PORT="$(agentd_smoke_pick_port)"
NAME="agentd_file_traversal_smoke"
SESSION_ID="file_traversal_${RANDOM}"
SESSIONS_ROOT="${LOG_DIR}/${NAME}_${PORT}.state"
SESSION_ROOT="${SESSIONS_ROOT}/session_${SESSION_ID}"

cleanup() {
  agentd_smoke_stop
  rm -rf "${SESSIONS_ROOT}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

mkdir -p "${SESSION_ROOT}/out"
echo -n "INSIDE" > "${SESSION_ROOT}/out/inside.txt"
echo -n "OUTSIDE" > "${SESSIONS_ROOT}/outside.txt"

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "${NAME}" \
  --no-yolo \
  --tools host

agentd_smoke_wait_health "${DAEMON_URL}"

qsid="$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote("${SESSION_ID}", safe=""))
PY
)"

qinside="$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote("out/inside.txt", safe=""))
PY
)"

body="$(curl -fsS --noproxy "*" --max-time 5 "${DAEMON_URL}/api/v1/file?session_id=${qsid}&path=${qinside}")"
if [[ "${body}" != "INSIDE" ]]; then
  echo "unexpected session file body: ${body}" >&2
  exit 1
fi

qescape="$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote("../outside.txt", safe=""))
PY
)"

resp="$(curl -sS --noproxy "*" --max-time 5 -w $'\n%{http_code}' \
  "${DAEMON_URL}/api/v1/file?session_id=${qsid}&path=${qescape}")"
body="${resp%$'\n'*}"
code="${resp##*$'\n'}"

if [[ "${code}" != "400" ]]; then
  echo "expected 400 for traversal; got ${code} body=${body}" >&2
  exit 1
fi
if [[ "${body}" != *"path escapes session root"* ]]; then
  echo "unexpected traversal body: ${body}" >&2
  exit 1
fi
