#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "missing agentd binary path arg" >&2
  exit 2
fi

if [[ "${AGENT_DISABLE_NETWORK_TESTS:-}" == "1" ]]; then
  echo "SKIP: AGENT_DISABLE_NETWORK_TESTS=1" >&2
  exit 77
fi

OPENROUTER_KEY="${OPENROUTER_API_KEY:-}"
source "${SCRIPT_DIR}/test_keys.sh"
agent_test_setup_proxy_env
OPENROUTER_KEY="$(agent_test_get_key openrouter 2>/dev/null || true)"
if [[ -z "${OPENROUTER_KEY}" ]]; then
  echo "SKIP: OPENROUTER_API_KEY not set and not found in project.local.md" >&2
  exit 77
fi

HOST="127.0.0.1"
PORT="$(agentd_smoke_pick_port)"

trap agentd_smoke_stop EXIT

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

OPENROUTER_API_KEY="${OPENROUTER_KEY}" agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "agentd_openrouter_models_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

resp="$(curl -fsS --noproxy "*" --max-time 45 \
  "${DAEMON_URL}/api/v1/openrouter/models?min_total=0.01&max_total=0.50&require_multimodal_input=1&require_tools=1&limit=20&refresh=1")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp}''')
if not obj.get("ok"):
  print(obj, file=sys.stderr)
  raise SystemExit(1)
models = obj.get("models") or []
if not isinstance(models, list) or len(models) == 0:
  print("expected non-empty models list", file=sys.stderr)
  raise SystemExit(1)
rec = obj.get("recommended_model") or ""
if not isinstance(rec, str) or not rec:
  print("missing recommended_model", file=sys.stderr)
  raise SystemExit(1)
first = models[0]
if not isinstance(first, dict) or "id" not in first:
  print("invalid models[0] shape", file=sys.stderr)
  raise SystemExit(1)
PY
