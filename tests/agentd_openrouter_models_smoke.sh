#!/usr/bin/env bash
set -euo pipefail

AGENTD_BIN="${1:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "missing agentd binary path arg" >&2
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
# Do not proxy localhost daemon traffic.
export no_proxy="${no_proxy:-${NO_PROXY:-127.0.0.1,localhost}}"
export NO_PROXY="${no_proxy}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

OPENROUTER_KEY="${OPENROUTER_API_KEY:-}"
PROJECT_MD="${PROJECT_ROOT}/project.md"
if [[ -z "${OPENROUTER_KEY}" && -f "${PROJECT_MD}" ]]; then
  OPENROUTER_KEY="$(grep -F -- "- openrouter:" "${PROJECT_MD}" | head -n 1 | sed -E 's/.*- openrouter:[[:space:]]*//')"
fi
if [[ -z "${OPENROUTER_KEY}" ]]; then
  echo "SKIP: OPENROUTER_API_KEY not set and not found in project.md" >&2
  exit 77
fi

PORT="$(( (RANDOM % 20000) + 20000 ))"
HOST="127.0.0.1"
DAEMON_URL="http://${HOST}:${PORT}"

cleanup() {
  if [[ -n "${AGENTD_PID:-}" ]]; then
    kill -TERM "${AGENTD_PID}" >/dev/null 2>&1 || true
    for _ in $(seq 1 30); do
      if ! kill -0 "${AGENTD_PID}" >/dev/null 2>&1; then
        break
      fi
      sleep 0.1
    done
    if kill -0 "${AGENTD_PID}" >/dev/null 2>&1; then
      kill -KILL "${AGENTD_PID}" >/dev/null 2>&1 || true
    fi
    wait "${AGENTD_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

LOG_DIR="${PROJECT_ROOT}/build"
mkdir -p "${LOG_DIR}"

OPENROUTER_API_KEY="${OPENROUTER_KEY}" "${AGENTD_BIN}" \
  --host "${HOST}" \
  --port "${PORT}" \
  --tools none \
  > "${LOG_DIR}/agentd_openrouter_models_smoke.stdout.log" 2> "${LOG_DIR}/agentd_openrouter_models_smoke.stderr.log" &
AGENTD_PID=$!

# Wait for health endpoint.
for _ in $(seq 1 60); do
  if curl -fsS --noproxy "*" --max-time 2 "${DAEMON_URL}/api/v1/health" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done
if ! curl -fsS --noproxy "*" --max-time 2 "${DAEMON_URL}/api/v1/health" >/dev/null 2>&1; then
  echo "agentd did not become healthy: ${DAEMON_URL}" >&2
  exit 1
fi

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

