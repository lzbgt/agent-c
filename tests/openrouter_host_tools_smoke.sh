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
if [[ "${AGENT_TEST_SKIP_OPENROUTER:-}" == "1" ]]; then
  echo "SKIP: AGENT_TEST_SKIP_OPENROUTER=1" >&2
  exit 77
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

source "${PROJECT_ROOT}/tests/test_keys.sh"
agent_test_setup_proxy_env
OPENROUTER_KEY="$(agent_test_get_key openrouter 2>/dev/null || true)"
if [[ -z "${OPENROUTER_KEY}" ]]; then
  echo "SKIP: OPENROUTER_API_KEY not set and not found in project.local.md" >&2
  exit 77
fi

BASE_URL="${OPENROUTER_API_BASE:-https://openrouter.ai/api/v1}"
MODEL="${AGENT_TEST_OPENROUTER_TOOL_MODEL:-bytedance-seed/seed-1.6-flash}"

try_model() {
  local model="$1"
  "${AGENT_BIN}" run "Use the shell_exec tool to run: echo OK. Then return exactly: OK" \
    --no-session \
    --timeout-ms 45000 \
    --base-url "${BASE_URL}" \
    --api-key "${OPENROUTER_KEY}" \
    --model "${model}" \
    --tools host \
    --force-tool shell_exec \
    --require-tool-call \
    --max-steps 4
}

out=""

# Keep this smoke test bounded in time:
# - try selected model
# - retry once (transient network/provider issues)
# - fallback to a known-tools model
set +e
out="$(try_model "${MODEL}" 2>/dev/null)"
rc=$?
if [[ $rc -ne 0 || -z "${out}" ]]; then
  out="$(try_model "${MODEL}" 2>/dev/null)"
  rc=$?
fi
if [[ $rc -ne 0 || -z "${out}" ]]; then
  out="$(try_model "mistralai/mistral-small-3.1-24b-instruct" 2>/dev/null)"
  rc=$?
fi
set -e

if [[ -z "${out}" ]]; then
  echo "OpenRouter host-tools smoke failed: no candidate model succeeded. Set AGENT_TEST_OPENROUTER_TOOL_MODEL to a known-good tools-capable model." >&2
  exit 1
fi

# Some OpenRouter models occasionally add extra commentary even when instructed to return exactly "OK".
# Treat this as success when the first non-empty line is exactly "OK" (smoke-level robustness).
first_line="$(printf '%s\n' "${out}" | tr -d '\r' | awk 'NF {print; exit}')"
if [[ "${first_line}" != "OK" ]]; then
  echo "unexpected output: ${out}" >&2
  exit 1
fi
