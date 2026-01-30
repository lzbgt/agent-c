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

HOST="0.0.0.0"
PORT_FAIL="$(agentd_smoke_pick_port)"
PORT_ALLOW="$(agentd_smoke_pick_port)"

set +e
"${AGENTD_BIN}" --host "${HOST}" --port "${PORT_FAIL}" --tools host \
  > "${LOG_DIR}/agentd_non_loopback_guard.stdout.log" 2> "${LOG_DIR}/agentd_non_loopback_guard.stderr.log"
rc=$?
set -e
if [[ "${rc}" -eq 0 ]]; then
  echo "expected non-loopback unauth agentd to fail, got rc=0" >&2
  exit 1
fi

# With explicit override, daemon should start.
trap agentd_smoke_stop EXIT

agentd_smoke_start_bind "${AGENTD_BIN}" "${HOST}" "127.0.0.1" "${PORT_ALLOW}" "agentd_non_loopback_allow" \
  --tools host \
  --allow-unauth

agentd_smoke_wait_health "${DAEMON_URL}"
exit 0
