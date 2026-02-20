#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<'USAGE'
Usage: tools/submit_agentd_parallel_demo.sh [options]

Options:
  --base <url>         agentd base URL (default: from out/devstack_state.json or http://127.0.0.1:8123)
  --token <token>      agentd auth token (default: AGENTD_AUTH_TOKEN or dev-agentd-token)
  --state <path>       devstack_state.json path (default: out/devstack_state.json)
  --targets <csv>      comma-separated agentd base URLs (overrides --state targets)
  --output <path>      output workflow JSON (default: out/workflows/agentd_parallel_demo.json)
  --goal <text>        workflow input goal
  --timeout-ms <n>     agentd_call timeout (default: 120000)
  --poll-ms <n>        agentd_call poll interval (default: 200)
  --bearer-env <name>  bearer env var (default: AGENTD_CALL_BEARER)
  --dry-run            generate JSON but do not submit
  -h, --help           show this help
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

AGENTD_BASE="${AGENTD_BASE:-}"
AGENTD_TOKEN="${AGENTD_AUTH_TOKEN:-dev-agentd-token}"
STATE_PATH="${ROOT}/out/devstack_state.json"
TARGETS=""
OUT_PATH="${ROOT}/out/workflows/agentd_parallel_demo.json"
GOAL="Draft a collaborative plan for a multi-agent workflow graph demo."
TIMEOUT_MS=120000
POLL_MS=200
BEARER_ENV="AGENTD_CALL_BEARER"
DRY_RUN=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --base) AGENTD_BASE="$2"; shift 2 ;;
    --token) AGENTD_TOKEN="$2"; shift 2 ;;
    --state) STATE_PATH="$2"; shift 2 ;;
    --targets) TARGETS="$2"; shift 2 ;;
    --output) OUT_PATH="$2"; shift 2 ;;
    --goal) GOAL="$2"; shift 2 ;;
    --timeout-ms) TIMEOUT_MS="$2"; shift 2 ;;
    --poll-ms) POLL_MS="$2"; shift 2 ;;
    --bearer-env) BEARER_ENV="$2"; shift 2 ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown arg: $1" >&2; usage; exit 1 ;;
  esac
done

if [[ -z "${AGENTD_BASE}" ]]; then
  AGENTD_BASE="$(python3 - <<PY
import json
from pathlib import Path
path = Path("${STATE_PATH}")
if not path.exists():
  print("http://127.0.0.1:8123")
  raise SystemExit(0)
try:
  data = json.loads(path.read_text())
except Exception:
  print("http://127.0.0.1:8123")
  raise SystemExit(0)
print(data.get("agentd_base") or "http://127.0.0.1:8123")
PY
)"
fi

if ! curl -fsS "${AGENTD_BASE}/api/v1/health" >/dev/null 2>&1; then
  echo "[submit-demo] warning: ${AGENTD_BASE} not reachable; falling back to http://127.0.0.1:8123" >&2
  AGENTD_BASE="http://127.0.0.1:8123"
fi

GEN_ARGS=(
  --state "${STATE_PATH}"
  --output "${OUT_PATH}"
  --goal "${GOAL}"
  --timeout-ms "${TIMEOUT_MS}"
  --poll-ms "${POLL_MS}"
  --bearer-env "${BEARER_ENV}"
)
if [[ -n "${TARGETS}" ]]; then
  GEN_ARGS+=(--targets "${TARGETS}")
fi

"${ROOT}/tools/gen_agentd_parallel_demo.sh" "${GEN_ARGS[@]}" >/dev/null

if [[ "${DRY_RUN}" -eq 1 ]]; then
  echo "[submit-demo] dry-run: ${OUT_PATH}"
  echo "[submit-demo] agentd_base: ${AGENTD_BASE}"
  exit 0
fi

curl -fsS \
  -H "Authorization: Bearer ${AGENTD_TOKEN}" \
  -H "Content-Type: application/json" \
  -d @"${OUT_PATH}" \
  "${AGENTD_BASE}/api/v1/workflow/submit"
