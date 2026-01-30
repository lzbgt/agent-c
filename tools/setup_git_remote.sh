#!/usr/bin/env bash
set -euo pipefail

# Purpose:
# - This repo's automation wants to `git push` after commits.
# - In some local workspaces, no remote is configured, which makes push impossible.
# - This helper configures `origin` from an env var or CLI arg and optionally pushes.
#
# Usage:
#   AGENT_GIT_REMOTE_URL="git@github.com:you/agent.git" tools/setup_git_remote.sh
#   tools/setup_git_remote.sh --url "git@github.com:you/agent.git" --push
#
# Notes:
# - This script does not guess a remote URL.
# - It can optionally load a URL from a gitignored `project.local.md` entry:
#     - git_remote: <url>

URL="${AGENT_GIT_REMOTE_URL:-}"
DO_PUSH=0
FORCE=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --url)
      URL="${2:-}"
      shift 2
      ;;
    --push)
      DO_PUSH=1
      shift 1
      ;;
    --force)
      FORCE=1
      shift 1
      ;;
    -h|--help)
      echo "Usage: setup_git_remote.sh [--url <remote>] [--push] [--force]"
      exit 0
      ;;
    *)
      echo "Unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

if [[ -z "${URL}" && -f "${ROOT}/project.local.md" ]]; then
  # Allow simple "YAML-ish" lines:
  #   - git_remote: <url>
  #   git_remote: <url>
  URL="$(grep -E '^[[:space:]]*-?[[:space:]]*git_remote[[:space:]]*:' "${ROOT}/project.local.md" \
    | head -n 1 \
    | sed -E 's/^[[:space:]]*-?[[:space:]]*git_remote[[:space:]]*:[[:space:]]*//')"
fi

if git remote -v | grep -Eq '^origin[[:space:]]'; then
  if [[ -n "${URL}" && "${FORCE}" == "1" ]]; then
    git remote set-url origin "${URL}"
    echo "Updated origin -> ${URL}"
  else
    echo "origin remote already configured:"
    git remote -v | grep -E '^origin[[:space:]]' || true
  fi
else
  if [[ -z "${URL}" ]]; then
    echo "No git remote configured." >&2
    echo "Provide one explicitly via:" >&2
    echo "  - env: AGENT_GIT_REMOTE_URL" >&2
    echo "  - flag: --url <remote>" >&2
    echo "  - file: project.local.md entry: '- git_remote: <url>'" >&2
    exit 1
  fi
  git remote add origin "${URL}"
  echo "Configured origin -> ${URL}"
fi

if [[ "${DO_PUSH}" == "1" ]]; then
  # Push current branch to origin.
  branch="$(git rev-parse --abbrev-ref HEAD)"
  echo "Pushing ${branch} -> origin/${branch}"
  git push -u origin "${branch}"
fi
