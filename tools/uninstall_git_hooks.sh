#!/usr/bin/env bash
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
HOOKS_PATH="$(git -C "${ROOT}" config core.hooksPath || echo ".git/hooks")"
if [[ "${HOOKS_PATH}" != /* ]]; then
  HOOKS_PATH="${ROOT}/${HOOKS_PATH}"
fi

FORCE=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --force)
      FORCE=1
      shift 1
      ;;
    -h|--help)
      cat <<'EOF'
Usage: tools/uninstall_git_hooks.sh [--force]

Removes the pre-commit hook installed by tools/install_git_hooks.sh.
If the hook doesn't look like ours, this script will refuse unless --force
is provided.
EOF
      exit 0
      ;;
    *)
      echo "Unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

HOOK_FILE="${HOOKS_PATH}/pre-commit"
if [[ ! -f "${HOOK_FILE}" ]]; then
  echo "[hooks] No pre-commit hook found at ${HOOK_FILE}."
  exit 0
fi

if grep -q "vendored_guard.py" "${HOOK_FILE}"; then
  rm -f "${HOOK_FILE}"
  echo "[hooks] Removed pre-commit hook at ${HOOK_FILE}"
  exit 0
fi

if [[ "${FORCE}" -eq 1 ]]; then
  rm -f "${HOOK_FILE}"
  echo "[hooks] Removed pre-commit hook at ${HOOK_FILE} (forced)."
  exit 0
fi

echo "[hooks] pre-commit hook does not match vendored guard; not removing."
echo "[hooks] Re-run with --force to remove it anyway."
exit 1
