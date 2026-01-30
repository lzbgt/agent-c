#!/usr/bin/env bash
set -euo pipefail

# Publish helper:
# - Runs local verification
# - Ensures `origin` exists (does not guess a URL)
# - Pushes the current branch

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

VERIFY_ARGS=()
FORCE_REMOTE=0
SKIP_UI=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-ui)
      SKIP_UI=1
      shift 1
      ;;
    --force-remote)
      FORCE_REMOTE=1
      shift 1
      ;;
    --verify-arg)
      VERIFY_ARGS+=("${2:-}")
      shift 2
      ;;
    -h|--help)
      cat <<'EOF'
Usage: tools/publish.sh [--skip-ui] [--force-remote] [--verify-arg <arg>]

Runs `tools/verify.sh` then pushes the current branch to `origin`.

Remote configuration:
  - Uses existing `origin` if present
  - Otherwise reads remote URL from:
      - env: AGENT_GIT_REMOTE_URL
      - gitignored file: project.local.md entry: '- git_remote: <url>'
  - Does not guess URLs

Flags:
  --skip-ui        Passes --skip-ui to tools/verify.sh
  --force-remote   If origin exists and AGENT_GIT_REMOTE_URL/project.local.md provides a URL,
                   updates origin to that URL (via tools/setup_git_remote.sh --force)
  --verify-arg X   Forward extra args to tools/verify.sh (repeatable)
EOF
      exit 0
      ;;
    *)
      echo "Unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

if [[ "${SKIP_UI}" == "1" ]]; then
  VERIFY_ARGS+=("--skip-ui")
fi

echo "[publish] verify: tools/verify.sh ${VERIFY_ARGS[*]:-}"
tools/verify.sh "${VERIFY_ARGS[@]}"

remote_args=()
if [[ "${FORCE_REMOTE}" == "1" ]]; then
  remote_args+=("--force")
fi

echo "[publish] setup remote + push"
tools/setup_git_remote.sh "${remote_args[@]}" --push

