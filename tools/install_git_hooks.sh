#!/usr/bin/env bash
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
HOOKS_PATH="$(git -C "${ROOT}" config core.hooksPath || echo ".git/hooks")"
if [[ "${HOOKS_PATH}" != /* ]]; then
  HOOKS_PATH="${ROOT}/${HOOKS_PATH}"
fi

FORCE=0
VERBOSE=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --force)
      FORCE=1
      shift 1
      ;;
    --verbose)
      VERBOSE=1
      shift 1
      ;;
    -h|--help)
      cat <<'EOF'
Usage: tools/install_git_hooks.sh [--force] [--verbose]

Installs a pre-commit hook that runs vendored subtree guard.
If a pre-commit hook already exists, this script will not overwrite it unless
--force is provided.
Respects core.hooksPath when set.
Use --verbose to hardcode verbose vendored guard output in the hook.
EOF
      exit 0
      ;;
    *)
      echo "Unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

mkdir -p "${HOOKS_PATH}"
HOOK_FILE="${HOOKS_PATH}/pre-commit"

if [[ -f "${HOOK_FILE}" ]] && [[ "${FORCE}" -ne 1 ]]; then
  echo "[hooks] ${HOOK_FILE} already exists; not overwriting."
  echo "[hooks] Add this line manually if you want the vendored guard:"
  echo "  python3 \"${ROOT}/tools/vendored_guard.py\" --path ref"
  exit 1
fi

if [[ "${VERBOSE}" -eq 1 ]]; then
  cat > "${HOOK_FILE}" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
ARGS=("--verbose")
if [[ "${VENDORED_GUARD_QUIET:-0}" == "1" ]]; then
  ARGS=(--quiet)
fi
python3 "${ROOT}/tools/vendored_guard.py" --path ref "${ARGS[@]}"
EOF
else
  cat > "${HOOK_FILE}" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
ARGS=()
if [[ "${VENDORED_GUARD_VERBOSE:-0}" == "1" ]]; then
  ARGS+=(--verbose)
elif [[ "${VENDORED_GUARD_QUIET:-0}" == "1" ]]; then
  ARGS+=(--quiet)
fi
python3 "${ROOT}/tools/vendored_guard.py" --path ref "${ARGS[@]}"
EOF
fi

chmod +x "${HOOK_FILE}"
echo "[hooks] Installed pre-commit hook at ${HOOK_FILE}"
