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

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

DEEPSEEK_KEY="${DEEPSEEK_API_KEY:-}"
source "${PROJECT_ROOT}/tests/test_keys.sh"
agent_test_setup_proxy_env
DEEPSEEK_KEY="$(agent_test_get_key deepseek 2>/dev/null || true)"
if [[ -z "${DEEPSEEK_KEY}" ]]; then
  echo "SKIP: DEEPSEEK_API_KEY not set and not found in project.local.md" >&2
  exit 77
fi

BASE_URL="${DEEPSEEK_API_BASE:-https://api.deepseek.com}"
MODEL="${AGENT_TEST_DEEPSEEK_TOOL_MODEL:-deepseek-chat}"

tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/agent_deepseek_fs_read_XXXXXX")"
cleanup() {
  rm -rf "${tmpdir}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

cat > "${tmpdir}/hello.txt" <<'EOF'
line1
TOKEN_OK_123
line3
EOF

# Deterministic-ish verification: force fs_read, and instruct arguments tightly.
out="$("${AGENT_BIN}" run "Use fs_read to read path: hello.txt starting at line 2, max_lines 1, max_chars 200. Then return exactly the token on that line (no quotes, no extra text)." \
  --no-session \
  --timeout-ms 45000 \
  --base-url "${BASE_URL}" \
  --api-key "${DEEPSEEK_KEY}" \
  --model "${MODEL}" \
  --tools host \
  --tools-root "${tmpdir}" \
  --force-tool fs_read \
  --require-tool-call \
  --max-steps 4)"

trimmed="$(echo "${out}" | tr -d '\r' | awk '{$1=$1;print}')"
if [[ "${trimmed}" != "TOKEN_OK_123" ]]; then
  echo "unexpected output: ${out}" >&2
  exit 1
fi
