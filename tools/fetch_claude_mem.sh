#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET="${ROOT}/ref/claude-mem"
REPO_URL="https://github.com/thedotmack/claude-mem.git"

if [[ -d "${TARGET}" ]]; then
  rm -rf "${TARGET}"
fi

git clone --depth 1 "${REPO_URL}" "${TARGET}"

# Record source metadata for reproducibility.
if [[ -d "${TARGET}/.git" ]]; then
  COMMIT="$(git -C "${TARGET}" rev-parse HEAD)"
else
  COMMIT=""
fi
cat <<META > "${TARGET}/.source.json"
{"repo_url":"${REPO_URL}","commit":"${COMMIT}","fetched_utc":"$(date -u +%Y-%m-%dT%H:%M:%SZ)"}
META
