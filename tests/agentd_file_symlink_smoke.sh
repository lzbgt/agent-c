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

PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

HOST="127.0.0.1"
PORT="$(agentd_smoke_pick_port)"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${AGENTD_PID:-}" ]]; then
    :
  fi
  rm -f "${PROJECT_ROOT}/build/agentd_file_symlink_out" >/dev/null 2>&1 || true
  rm -rf "${TMPDIR_CREATED:-}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

TMPDIR_CREATED="$(mktemp -d)"
echo "SECRET_FILE" > "${TMPDIR_CREATED}/secret.txt"

# Create a symlink inside the daemon host scope (PROJECT_ROOT) pointing outside.
ln -s "${TMPDIR_CREATED}" "${PROJECT_ROOT}/build/agentd_file_symlink_out" 2>/dev/null || true
if [[ ! -L "${PROJECT_ROOT}/build/agentd_file_symlink_out" ]]; then
  echo "SKIP: cannot create symlink in this environment" >&2
  exit 77
fi

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "agentd_file_symlink_smoke" \
  --no-yolo \
  --tools host

agentd_smoke_wait_health "${DAEMON_URL}"

# File endpoint: no tools_root/host_scope sandboxing; reading through a symlink is allowed.
qpath="$(python3 - <<'PY'
import urllib.parse
print(urllib.parse.quote('build/agentd_file_symlink_out/secret.txt'))
PY
)"
body="$(curl -fsS --noproxy "*" --max-time 5 "${DAEMON_URL}/api/v1/file?path=${qpath}")"

if [[ "${body}" != "SECRET_FILE" ]]; then
  echo "unexpected file body: ${body}" >&2
  exit 1
fi
