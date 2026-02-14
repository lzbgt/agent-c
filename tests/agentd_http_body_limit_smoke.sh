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

PORT_BODY="$(agentd_smoke_pick_port)"
PORT_HEADER="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"

cleanup() {
  unset AGENTD_HTTP_MAX_BODY_BYTES
  unset AGENTD_HTTP_MAX_HEADER_BYTES
  agentd_smoke_stop
}
trap cleanup EXIT

export AGENTD_HTTP_MAX_BODY_BYTES=256
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_BODY}" "agentd_http_body_limit_smoke" \
  --tools none \
  --no-yolo

agentd_smoke_wait_health "${DAEMON_URL}"

tmp_body="${LOG_DIR}/agentd_http_body_limit_body.json"
python3 - <<'PY' > "${tmp_body}"
import json
print(json.dumps({
  "prompt": "x" * 1024,
  "no_session": True,
  "tools": "none"
}))
PY

code="$(curl -sS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "@${tmp_body}" \
  -o "${LOG_DIR}/agentd_http_body_limit_response.json" \
  -w "%{http_code}" \
  "${DAEMON_URL}/api/v1/run")"

if [[ "${code}" != "413" ]]; then
  echo "expected 413, got ${code}" >&2
  exit 1
fi

if ! grep -q "request body too large" "${LOG_DIR}/agentd_http_body_limit_response.json"; then
  echo "missing expected error message" >&2
  cat "${LOG_DIR}/agentd_http_body_limit_response.json" >&2
  exit 1
fi

agentd_smoke_stop
unset AGENTD_HTTP_MAX_BODY_BYTES

export AGENTD_HTTP_MAX_HEADER_BYTES=256
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_HEADER}" "agentd_http_header_limit_smoke" \
  --tools none \
  --no-yolo

agentd_smoke_wait_health "${DAEMON_URL}"

BIG_HEADER="$(python3 - <<'PY'
print("a" * 1024)
PY
)"

code="$(curl -sS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -H "X-Big-Header: ${BIG_HEADER}" \
  -d '{"prompt":"hi","no_session":true,"tools":"none"}' \
  -o "${LOG_DIR}/agentd_http_header_limit_response.json" \
  -w "%{http_code}" \
  "${DAEMON_URL}/api/v1/run")"

if [[ "${code}" != "431" ]]; then
  echo "expected 431, got ${code}" >&2
  cat "${LOG_DIR}/agentd_http_header_limit_response.json" >&2
  exit 1
fi

if ! grep -q "request header too large" "${LOG_DIR}/agentd_http_header_limit_response.json"; then
  echo "missing expected header limit error" >&2
  cat "${LOG_DIR}/agentd_http_header_limit_response.json" >&2
  exit 1
fi
