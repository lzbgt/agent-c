#!/usr/bin/env bash
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
HOOKS_PATH="$(git -C "${ROOT}" config core.hooksPath || echo ".git/hooks")"
if [[ "${HOOKS_PATH}" != /* ]]; then
  HOOKS_PATH="${ROOT}/${HOOKS_PATH}"
fi

HOOK_FILE="${HOOKS_PATH}/pre-commit"

echo "[hooks] repo: ${ROOT}"
echo "[hooks] hooksPath: ${HOOKS_PATH}"

if [[ ! -f "${HOOK_FILE}" ]]; then
  echo "[hooks] pre-commit: missing"
  exit 0
fi

echo "[hooks] pre-commit: ${HOOK_FILE}"

if grep -q "vendored_guard.py" "${HOOK_FILE}"; then
  echo "[hooks] vendored guard: installed"
  exit 0
fi

echo "[hooks] vendored guard: not detected"
