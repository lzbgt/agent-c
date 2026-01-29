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

DEEPSEEK_KEY="${DEEPSEEK_API_KEY:-}"
PROJECT_MD="${PROJECT_ROOT}/project.md"
if [[ -z "${DEEPSEEK_KEY}" && -f "${PROJECT_MD}" ]]; then
  DEEPSEEK_KEY="$(grep -F -- "- deepseek:" "${PROJECT_MD}" | head -n 1 | sed -E 's/.*- deepseek:[[:space:]]*//')"
fi
if [[ -z "${DEEPSEEK_KEY}" ]]; then
  echo "SKIP: DEEPSEEK_API_KEY not set and not found in project.md" >&2
  exit 77
fi

BASE_URL="${DEEPSEEK_API_BASE:-https://api.deepseek.com}"
MODEL="${AGENT_TEST_DEEPSEEK_TOOL_MODEL:-deepseek-chat}"

# This test uses forced tool selection (`--force-tool`) for deterministic verification.
# `deepseek-chat` supports this flow; `deepseek-reasoner` may reject forced tool_choice.
out="$("${AGENT_BIN}" run "Use the calculator tool to compute (2+2)*10. Return exactly: 40" \
  --no-session \
  --timeout-ms 30000 \
  --base-url "${BASE_URL}" \
  --api-key "${DEEPSEEK_KEY}" \
  --model "${MODEL}" \
  --tools basic \
  --force-tool calculator \
  --require-tool-call \
  --max-steps 6)"

trimmed="$(echo "${out}" | tr -d '\r' | awk '{$1=$1;print}')"
if [[ "${trimmed}" != "40" ]]; then
  echo "unexpected output: ${out}" >&2
  exit 1
fi
