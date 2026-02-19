#!/usr/bin/env bash
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
HOOKS_PATH="$(git -C "${ROOT}" config core.hooksPath || echo ".git/hooks")"
if [[ "${HOOKS_PATH}" != /* ]]; then
  HOOKS_PATH="${ROOT}/${HOOKS_PATH}"
fi

HOOK_FILE="${HOOKS_PATH}/pre-commit"
JSON=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --json)
      JSON=1
      shift 1
      ;;
    -h|--help)
      cat <<'EOF'
Usage: tools/hooks_status.sh [--json]

Prints pre-commit hook status and whether vendored guard is installed.
EOF
      exit 0
      ;;
    *)
      echo "Unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

has_hook=0
vendored_installed=0

if [[ -f "${HOOK_FILE}" ]]; then
  has_hook=1
  if grep -q "vendored_guard.py" "${HOOK_FILE}"; then
    vendored_installed=1
  fi
fi

if [[ "${JSON}" -eq 1 ]]; then
  printf '{'
  printf '"repo":"%s",' "${ROOT}"
  printf '"hooks_path":"%s",' "${HOOKS_PATH}"
  printf '"pre_commit":"%s",' "${HOOK_FILE}"
  printf '"pre_commit_present":%s,' "${has_hook}"
  printf '"vendored_guard_installed":%s' "${vendored_installed}"
  printf '}\n'
  exit 0
fi

echo "[hooks] repo: ${ROOT}"
echo "[hooks] hooksPath: ${HOOKS_PATH}"

if [[ "${has_hook}" -eq 0 ]]; then
  echo "[hooks] pre-commit: missing"
  exit 0
fi

echo "[hooks] pre-commit: ${HOOK_FILE}"

if [[ "${vendored_installed}" -eq 1 ]]; then
  echo "[hooks] vendored guard: installed"
  exit 0
fi

echo "[hooks] vendored guard: not detected"
