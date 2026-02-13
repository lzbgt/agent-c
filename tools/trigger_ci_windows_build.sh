#!/usr/bin/env bash
set -euo pipefail

# Trigger the Windows build workflow via GitHub API or gh CLI.
# Requires GITHUB_TOKEN or GH_TOKEN, or a logged-in gh CLI session.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REMOTE_URL="$(git -C "${ROOT}" config --get remote.origin.url || true)"

if [[ -z "${REMOTE_URL}" ]]; then
  echo "missing git remote.origin.url" >&2
  exit 2
fi

repo=""
if [[ "${REMOTE_URL}" =~ github.com[:/]+([^/]+/[^/.]+)(\.git)?$ ]]; then
  repo="${BASH_REMATCH[1]}"
fi

if [[ -z "${repo}" ]]; then
  echo "failed to parse GitHub repo from remote: ${REMOTE_URL}" >&2
  exit 2
fi

ref="${1:-master}"
token="${GITHUB_TOKEN:-${GH_TOKEN:-}}"

if command -v gh >/dev/null 2>&1; then
  if gh auth status >/dev/null 2>&1; then
    gh workflow run windows-build.yml -R "${repo}" -r "${ref}"
    echo "dispatched via gh: repo=${repo} ref=${ref}"
    exit 0
  fi
fi

if [[ -z "${token}" ]]; then
  echo "missing GITHUB_TOKEN/GH_TOKEN and gh not authenticated" >&2
  exit 3
fi

api="https://api.github.com/repos/${repo}/actions/workflows/windows-build.yml/dispatches"
payload=$(python3 - <<PY
import json
print(json.dumps({"ref": "${ref}"}))
PY
)

http_code="$(
  curl -sS -o /dev/null -w "%{http_code}" \
    -H "Accept: application/vnd.github+json" \
    -H "Authorization: Bearer ${token}" \
    -X POST \
    -d "${payload}" \
    "${api}" || true
)"

if [[ "${http_code}" != "204" ]]; then
  echo "failed to dispatch workflow (http=${http_code})" >&2
  exit 4
fi

echo "dispatched via API: repo=${repo} ref=${ref}"
