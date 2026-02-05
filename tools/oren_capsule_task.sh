#!/usr/bin/env bash
set -euo pipefail

# Build an Oren `.obc` capsule from a `.oren` source file, then emit a ready-to-paste
# `avm_capsule` workflow task JSON object for agentd.
#
# This is a bring-up helper that bridges:
#   oren-lang (compiler)  -> .obc bytes
#   agentd (workflow)    -> kind: "avm_capsule" tasks
#
# Usage:
#   tools/oren_capsule_task.sh --src /abs/path/to/prog.oren --task-id AVM
#
# Env:
#   OREN_LANG_ROOT   Absolute path to oren-lang repo (default: ../oren-lang relative to this repo)
#
# Output:
#   Prints a single JSON object (a workflow task spec) to stdout.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

SRC=""
TASK_ID="AVM"
OUT_OBC=""

TIMEOUT_MS="1000"
GAS="1000"
MEM_BYTES="64000"
IO_BYTES="0"
LOG_BYTES="0"
DETERMINISTIC="1"
ALLOW_DOMAINS=""

ONLY_BASE64=0
PRINT_OBC_PATH=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --src)
      SRC="${2:-}"; shift 2 ;;
    --task-id)
      TASK_ID="${2:-}"; shift 2 ;;
    --out-obc)
      OUT_OBC="${2:-}"; shift 2 ;;
    --timeout-ms)
      TIMEOUT_MS="${2:-}"; shift 2 ;;
    --gas)
      GAS="${2:-}"; shift 2 ;;
    --mem-bytes)
      MEM_BYTES="${2:-}"; shift 2 ;;
    --io-bytes)
      IO_BYTES="${2:-}"; shift 2 ;;
    --log-bytes)
      LOG_BYTES="${2:-}"; shift 2 ;;
    --deterministic)
      DETERMINISTIC="${2:-}"; shift 2 ;;
    --allow-domains)
      ALLOW_DOMAINS="${2:-}"; shift 2 ;;
    --only-base64)
      ONLY_BASE64=1; shift 1 ;;
    --print-obc-path)
      PRINT_OBC_PATH=1; shift 1 ;;
    -h|--help)
      cat <<'EOF'
Usage: tools/oren_capsule_task.sh --src <file.oren> [options]

Builds a `.obc` capsule using `../oren-lang/oren` and prints:
- by default: a single JSON object representing a workflow task:
    {"task_id":"...","kind":"avm_capsule","capsule":{...}}
- with --only-base64: prints only the base64-encoded `.obc` bytes

Required:
  --src <file.oren>         Path to a `.oren` source file.

Options:
  --task-id <id>            Workflow task_id (default: AVM)
  --out-obc <file.obc>      Output `.obc` path (default: repo build/tmp)
  --print-obc-path          Also print the absolute `.obc` path to stderr
  --only-base64             Output only the base64 string (no JSON wrapper)

Budgets / knobs (capsule args):
  --timeout-ms <ms>         Default 1000
  --gas <n>                 Default 1000
  --mem-bytes <n>           Default 64000
  --io-bytes <n>            Default 0
  --log-bytes <n>           Default 0
  --deterministic <0|1>     Default 1
  --allow-domains <csv>     Default empty

Env:
  OREN_LANG_ROOT            Absolute path to oren-lang repo (default: ../oren-lang)
EOF
      exit 0
      ;;
    *)
      echo "Unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

if [[ -z "${SRC}" ]]; then
  echo "ERROR: missing --src <file.oren>" >&2
  exit 2
fi
if [[ ! -f "${SRC}" ]]; then
  echo "ERROR: --src does not exist: ${SRC}" >&2
  exit 2
fi

ABS_SRC="$(python3 - <<PY
import os
print(os.path.realpath(r'''${SRC}'''))
PY
)"

OREN_LANG_ROOT="${OREN_LANG_ROOT:-}"
if [[ -z "${OREN_LANG_ROOT}" ]]; then
  OREN_LANG_ROOT="$(cd "${ROOT}/../oren-lang" 2>/dev/null && pwd || true)"
fi
if [[ -z "${OREN_LANG_ROOT}" || ! -d "${OREN_LANG_ROOT}" ]]; then
  echo "ERROR: oren-lang repo not found. Set OREN_LANG_ROOT=/abs/path/to/oren-lang" >&2
  exit 1
fi

OREN_BIN="${OREN_LANG_ROOT}/oren"
if [[ "$(uname -s 2>/dev/null || true)" == *"MINGW"* || "$(uname -s 2>/dev/null || true)" == *"MSYS"* ]]; then
  if [[ -f "${OREN_LANG_ROOT}/oren.exe" ]]; then
    OREN_BIN="${OREN_LANG_ROOT}/oren.exe"
  fi
fi

mkdir -p "${ROOT}/build/tmp"
if [[ -z "${OUT_OBC}" ]]; then
  base="$(basename "${SRC}")"
  OUT_OBC="${ROOT}/build/tmp/${base%.oren}.obc"
fi

ts="$(date +%Y%m%d_%H%M%S)"
log="${ROOT}/build/oren_capsule_task_${ts}.log"

if [[ ! -x "${OREN_BIN}" ]]; then
  echo "[oren-capsule] building oren in ${OREN_LANG_ROOT} (log: ${log})" >&2
  (
    cd "${OREN_LANG_ROOT}"
    make oren
  ) >"${log}" 2>&1 || {
    echo "[oren-capsule] FAILED to build oren (see log): ${log}" >&2
    tail -n 160 "${log}" >&2 || true
    exit 1
  }
fi

if [[ ! -x "${OREN_BIN}" ]]; then
  echo "ERROR: oren binary missing or not executable at ${OREN_BIN}" >&2
  echo "Hint: check build log: ${log}" >&2
  exit 1
fi

mkdir -p "$(dirname "${OUT_OBC}")"
(
  cd "${OREN_LANG_ROOT}"
  "${OREN_BIN}" build "${ABS_SRC}" --backend bytecode -o "${OUT_OBC}"
) >>"${log}" 2>&1 || {
  echo "[oren-capsule] FAILED to build .obc (see log): ${log}" >&2
  tail -n 200 "${log}" >&2 || true
  exit 1
}

if [[ ! -f "${OUT_OBC}" ]]; then
  echo "ERROR: expected output .obc missing: ${OUT_OBC}" >&2
  exit 1
fi

abs_obc="$(python3 - <<PY
import os
print(os.path.realpath(r'''${OUT_OBC}'''))
PY
)"

if [[ "${PRINT_OBC_PATH}" == "1" ]]; then
  echo "[oren-capsule] obc: ${abs_obc}" >&2
fi

obc_b64="$(python3 - <<PY
import base64
with open(r'''${OUT_OBC}''','rb') as f:
  b = f.read()
print(base64.b64encode(b).decode('ascii'))
PY
)"

if [[ "${ONLY_BASE64}" == "1" ]]; then
  echo "${obc_b64}"
  exit 0
fi

TASK_ID_ENV="${TASK_ID}" \
OBC_B64_ENV="${obc_b64}" \
TIMEOUT_MS_ENV="${TIMEOUT_MS}" \
GAS_ENV="${GAS}" \
MEM_BYTES_ENV="${MEM_BYTES}" \
IO_BYTES_ENV="${IO_BYTES}" \
LOG_BYTES_ENV="${LOG_BYTES}" \
DETERMINISTIC_ENV="${DETERMINISTIC}" \
ALLOW_DOMAINS_ENV="${ALLOW_DOMAINS}" \
python3 - <<'PY'
import json, os

task = {
  "task_id": os.environ["TASK_ID_ENV"],
  "kind": "avm_capsule",
  "capsule": {
    "obc_base64": os.environ["OBC_B64_ENV"],
    "timeout_ms": int(os.environ["TIMEOUT_MS_ENV"]),
    "gas": int(os.environ["GAS_ENV"]),
    "mem_bytes": int(os.environ["MEM_BYTES_ENV"]),
    "io_bytes": int(os.environ["IO_BYTES_ENV"]),
    "log_bytes": int(os.environ["LOG_BYTES_ENV"]),
    "deterministic": bool(int(os.environ["DETERMINISTIC_ENV"])),
  },
}
allow = (os.environ.get("ALLOW_DOMAINS_ENV") or "").strip()
if allow:
  task["capsule"]["allow_domains"] = allow
print(json.dumps(task))
PY
