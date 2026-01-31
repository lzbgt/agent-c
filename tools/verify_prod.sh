#!/usr/bin/env bash
set -euo pipefail

# Production-ish verification helper:
# - sources ~/.env (if present) so real provider API keys (e.g. DEEPSEEK_API_KEY) are available
# - then runs the standard verification pipeline
#
# This is intentionally best-effort and local-only: do NOT commit secrets into this repo.

if [[ -f "${HOME}/.env" ]]; then
  # Export vars from ~/.env into the environment of subprocesses (ctest smoke tests, etc.).
  # shellcheck disable=SC1090
  set -a
  source "${HOME}/.env"
  set +a
fi

exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/verify.sh" "$@"

