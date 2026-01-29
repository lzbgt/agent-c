#!/usr/bin/env bash
set -euo pipefail

AGENT_BIN="${1:-}"
if [[ -z "${AGENT_BIN}" ]]; then
  echo "missing agent binary path arg" >&2
  exit 2
fi

if [[ "${AGENT_DISABLE_NETWORK_TESTS:-}" == "1" ]]; then
  echo "SKIP: AGENT_DISABLE_NETWORK_TESTS=1" >&2
  exit 77
fi

# Always set proxy for network tests (can be required in some environments).
# The HTTP client may still fall back to direct if the proxy is unreachable.
export https_proxy="http://localhost:8120"
export http_proxy="http://localhost:8120"
export HTTPS_PROXY="${https_proxy}"
export HTTP_PROXY="${http_proxy}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

OPENROUTER_KEY="${OPENROUTER_API_KEY:-}"
PROJECT_MD="${PROJECT_ROOT}/project.md"
if [[ -z "${OPENROUTER_KEY}" && -f "${PROJECT_MD}" ]]; then
  # project.md contains non-sensitive test keys in the form:
  # - openrouter: sk-or-v1-...
  OPENROUTER_KEY="$(grep -F -- "- openrouter:" "${PROJECT_MD}" | head -n 1 | sed -E 's/.*- openrouter:[[:space:]]*//')"
fi
if [[ -z "${OPENROUTER_KEY}" ]]; then
  echo "SKIP: OPENROUTER_API_KEY not set and not found in project.md" >&2
  exit 77
fi

BASE_URL="${OPENROUTER_API_BASE:-https://openrouter.ai/api/v1}"
MODEL="${AGENT_TEST_OPENROUTER_MODEL:-bytedance-seed/seed-1.6-flash}"

try_model() {
  local model="$1"
  "${AGENT_BIN}" run "Return exactly: OK" --no-session --tools none --timeout-ms 45000 --base-url "${BASE_URL}" --api-key "${OPENROUTER_KEY}" --model "${model}"
}

out=""

# Keep this smoke test bounded in time: try the selected model once, then one fallback.
set +e
out="$(try_model "${MODEL}" 2>/dev/null)"
rc=$?
set -e
if [[ $rc -ne 0 || -z "${out}" ]]; then
  set +e
  out="$(try_model "google/gemma-3-4b-it" 2>/dev/null)"
  rc=$?
  set -e
  if [[ $rc -ne 0 || -z "${out}" ]]; then
    out=""
  fi
fi

if [[ -z "${out}" ]]; then
  echo "OpenRouter smoke failed: no candidate model succeeded. Set AGENT_TEST_OPENROUTER_MODEL to a known-good model." >&2
  exit 1
fi
trimmed="$(echo "${out}" | tr -d '\r' | awk '{$1=$1;print}')"

if [[ "${trimmed}" != "OK" ]]; then
  echo "unexpected output: ${out}" >&2
  exit 1
fi
