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
# - This script does not guess a remote URL. Provide one explicitly.

URL="${AGENT_GIT_REMOTE_URL:-}"
DO_PUSH=0

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
    -h|--help)
      echo "Usage: setup_git_remote.sh [--url <remote>] [--push]"
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

if git remote -v | rg -q '^origin\\s'; then
  echo "origin remote already configured:"
  git remote -v | rg '^origin\\s' || true
else
  if [[ -z "${URL}" ]]; then
    echo "No git remote configured." >&2
    echo "Set AGENT_GIT_REMOTE_URL or pass --url to configure origin." >&2
    echo "Example: AGENT_GIT_REMOTE_URL='git@github.com:you/agent.git' tools/setup_git_remote.sh --push" >&2
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

