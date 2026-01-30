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

tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/agent_deepseek_fs_find_XXXXXX")"
cleanup() {
  rm -rf "${tmpdir}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

mkdir -p "${tmpdir}/src" "${tmpdir}/node_modules/pkg"
cat > "${tmpdir}/src/TOKEN_OK_789.cpp" <<'EOF'
int main() { return 0; }
EOF
cat > "${tmpdir}/node_modules/pkg/ignore.js" <<'EOF'
console.log("ignore");
EOF

# Deterministic-ish verification: force fs_find and ask the model to return exactly the found path.
out="$("${AGENT_BIN}" run "Use fs_find to search under path '.' recursively. Use name_substring: TOKEN_OK_789 and extensions: ['.cpp'] (or [\".cpp\"]). Keep use_default_excludes=true. Then return exactly the found path (no quotes, no extra text)." \
  --no-session \
  --timeout-ms 45000 \
  --base-url "${BASE_URL}" \
  --api-key "${DEEPSEEK_KEY}" \
  --model "${MODEL}" \
  --tools host \
  --tools-root "${tmpdir}" \
  --force-tool fs_find \
  --require-tool-call \
  --max-steps 4)"

out_norm="$(echo "${out}" | tr -d '\r')"
if echo "${out_norm}" | grep -q '^src/TOKEN_OK_789\\.cpp$'; then
  exit 0
fi
if echo "${out_norm}" | grep -Fq 'src/TOKEN_OK_789.cpp'; then
  exit 0
fi

echo "unexpected output: ${out}" >&2
exit 1
