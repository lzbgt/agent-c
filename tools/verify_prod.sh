#!/usr/bin/env bash
set -euo pipefail

# Production-ish verification helper:
# - sources ~/.env (if present) so real provider API keys (e.g. DEEPSEEK_API_KEY) are available
# - then runs the standard verification pipeline
#
# This is intentionally best-effort and local-only: do NOT commit secrets into this repo.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/lib/agent_env.sh
source "${ROOT}/tools/lib/agent_env.sh"
agent_env_source_home >/dev/null 2>&1 || true

exec "${ROOT}/tools/verify.sh" "$@"
