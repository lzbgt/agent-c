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
export https_proxy="${https_proxy:-${HTTPS_PROXY:-http://localhost:8120}}"
export http_proxy="${http_proxy:-${HTTP_PROXY:-http://localhost:8120}}"
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

