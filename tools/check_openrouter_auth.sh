#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT_DIR="${ROOT}/tests"

# shellcheck source=tests/test_keys.sh
source "${SCRIPT_DIR}/test_keys.sh"

agent_test_setup_proxy_env
OPENROUTER_KEY="$(agent_test_get_key openrouter 2>/dev/null || true)"
if [[ -z "${OPENROUTER_KEY}" ]]; then
  echo "SKIP: OPENROUTER_API_KEY not set and not found in .not_in_repo, project.local.md, or ~/.env" >&2
  exit 77
fi

BASE_URL="${OPENROUTER_API_BASE:-https://openrouter.ai/api/v1}"
source_hint="$(agent_test_get_key_source openrouter 2>/dev/null || true)"
if agent_test_openrouter_auth_ok "${OPENROUTER_KEY}" "${BASE_URL}"; then
  if [[ -n "${source_hint}" ]]; then
    echo "OK: OpenRouter auth succeeded (${BASE_URL}) [${source_hint}]"
  else
    echo "OK: OpenRouter auth succeeded (${BASE_URL})"
  fi
  exit 0
fi
exit $?
