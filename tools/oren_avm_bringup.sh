#!/usr/bin/env bash
set -euo pipefail

# Fast bring-up helper to produce an Oren AVM binary for the agentd AVM endpoints.
#
# Defaults:
# - Oren repo assumed at ../oren-lang relative to this repo root
# - Builds `avm` via `make avm` if missing
#
# Usage:
#   tools/oren_avm_bringup.sh
#   tools/oren_avm_bringup.sh --verify
#   OREN_LANG_ROOT=/abs/path/to/oren-lang tools/oren_avm_bringup.sh
#
# Output:
#   Prints the absolute path to the avm binary (suitable for AGENTD_AVM_BIN).

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

VERIFY=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --verify)
      VERIFY=1
      shift 1
      ;;
    -h|--help)
      cat <<'EOF'
Usage: tools/oren_avm_bringup.sh [--verify]

Builds the Oren AVM binary in ../oren-lang (or $OREN_LANG_ROOT) and prints its path.

Env:
  OREN_LANG_ROOT   Absolute path to oren-lang repo (default: ../oren-lang)

Flags:
  --verify         Also run oren-lang/scripts/verify_avm_bytecode_link_smoke.sh
EOF
      exit 0
      ;;
    *)
      echo "Unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

OREN_LANG_ROOT="${OREN_LANG_ROOT:-}"
if [[ -z "${OREN_LANG_ROOT}" ]]; then
  OREN_LANG_ROOT="$(cd "${ROOT}/../oren-lang" 2>/dev/null && pwd || true)"
fi

if [[ -z "${OREN_LANG_ROOT}" || ! -d "${OREN_LANG_ROOT}" ]]; then
  echo "ERROR: oren-lang repo not found. Set OREN_LANG_ROOT=/abs/path/to/oren-lang" >&2
  exit 1
fi

AVM_BIN="${OREN_LANG_ROOT}/avm"
if [[ "$(uname -s 2>/dev/null || true)" == *"MINGW"* || "$(uname -s 2>/dev/null || true)" == *"MSYS"* ]]; then
  if [[ -f "${OREN_LANG_ROOT}/avm.exe" ]]; then
    AVM_BIN="${OREN_LANG_ROOT}/avm.exe"
  fi
fi

mkdir -p "${ROOT}/build"
ts="$(date +%Y%m%d_%H%M%S)"
log="${ROOT}/build/oren_avm_bringup_${ts}.log"

if [[ ! -x "${AVM_BIN}" ]]; then
  echo "[oren-avm] building avm in ${OREN_LANG_ROOT} (log: ${log})" >&2
  (
    cd "${OREN_LANG_ROOT}"
    make avm
  ) >"${log}" 2>&1 || {
    echo "[oren-avm] FAILED (see log): ${log}" >&2
    tail -n 120 "${log}" >&2 || true
    exit 1
  }
fi

if [[ ! -x "${AVM_BIN}" ]]; then
  echo "ERROR: avm binary missing or not executable at ${AVM_BIN}" >&2
  echo "Hint: check build log: ${log}" >&2
  exit 1
fi

if [[ "${VERIFY}" == "1" ]]; then
  vts="$(date +%Y%m%d_%H%M%S)"
  vlog="${ROOT}/build/oren_avm_verify_${vts}.log"
  echo "[oren-avm] verify avm bytecode link smoke (log: ${vlog})" >&2
  (
    cd "${OREN_LANG_ROOT}"
    ./scripts/verify_avm_bytecode_link_smoke.sh
  ) >"${vlog}" 2>&1 || {
    echo "[oren-avm] VERIFY FAILED (see log): ${vlog}" >&2
    tail -n 120 "${vlog}" >&2 || true
    exit 1
  }
fi

python3 - <<PY
import os
print(os.path.realpath(r'''${AVM_BIN}'''))
PY

