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

expect_fail() {
  local label="$1"
  local needle="$2"
  shift 2
  local log_file="${LOG_DIR}/${label}.log"
  set +e
  "$@" >"${log_file}" 2>&1
  local rc=$?
  set -e
  if [[ ${rc} -eq 0 ]]; then
    echo "expected failure but command succeeded: $*" >&2
    return 1
  fi
  if ! rg -q --fixed-strings "${needle}" "${log_file}"; then
    echo "missing expected error message: ${needle}" >&2
    return 1
  fi
}

expect_fail "agentd_cli_invalid_tools" "Invalid --tools" "${AGENTD_BIN}" --tools bogus
expect_fail "agentd_cli_invalid_host_policy" "Invalid --host-policy" "${AGENTD_BIN}" --host-policy nope
expect_fail "agentd_cli_invalid_system_profile" "Invalid --system-profile" "${AGENTD_BIN}" --system-profile nope

echo "agentd_cli_validation_smoke OK"
