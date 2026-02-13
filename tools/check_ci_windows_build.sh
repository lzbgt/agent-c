#!/usr/bin/env bash
set -euo pipefail

# Check the latest GitHub Actions run for the Windows build workflow.
# Uses unauthenticated API by default (rate-limited). Set GITHUB_TOKEN to increase limits.

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

api="https://api.github.com/repos/${repo}/actions/workflows/windows-build.yml/runs?per_page=1&branch=master"
headers=(-H "Accept: application/vnd.github+json")
token="${GITHUB_TOKEN:-${GH_TOKEN:-}}"
if [[ -n "${token}" ]]; then
  headers+=(-H "Authorization: Bearer ${token}")
fi

tmp="$(mktemp)"
http_code="$(
  curl -sS -o "${tmp}" -w "%{http_code}" "${headers[@]}" "${api}" || true
)"

json=""
if [[ "${http_code}" == "200" ]]; then
  json="$(cat "${tmp}")"
elif command -v gh >/dev/null 2>&1; then
  if gh auth status >/dev/null 2>&1; then
    json="$(gh api -H "Accept: application/vnd.github+json" \
      "/repos/${repo}/actions/workflows/windows-build.yml/runs?per_page=1&branch=master" 2>/dev/null || true)"
  fi
fi
rm -f "${tmp}"

if [[ -z "${json}" ]]; then
  if [[ "${http_code}" == "404" ]]; then
    echo "failed to fetch workflow runs (repo=${repo}). If the repo is private, set GITHUB_TOKEN or GH_TOKEN." >&2
  else
    echo "failed to fetch workflow runs (repo=${repo}, http=${http_code}). Set GITHUB_TOKEN or GH_TOKEN to increase API access." >&2
  fi
  exit 3
fi

python3 - <<PY
import json, sys
data = json.loads(sys.stdin.read())
runs = data.get("workflow_runs") or []
if not runs:
  print("no workflow runs found")
  raise SystemExit(0)
run = runs[0]
print("workflow:", run.get("name"))
print("status:", run.get("status"))
print("conclusion:", run.get("conclusion"))
print("created_at:", run.get("created_at"))
print("html_url:", run.get("html_url"))
PY
<<<"${json}"
