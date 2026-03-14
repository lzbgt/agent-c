#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEFAULT_STATE="${ROOT}/out/devstack_state.json"
STATE_PATH="${AGENT_DEVSTACK_STATE:-${DEFAULT_STATE}}"
REQUIRE_LIVE=0
PACKS=()

usage() {
  cat <<'EOF'
Usage: tools/update_eval_baselines.sh [--state <path>] [--require-live] [pack...]

Refresh canonical eval-pack baselines under ref/eval_packs/.

Defaults:
  - always updates self-contained packs:
      tools/eval_packs/eval_pack_smoke.json
      tools/eval_packs/eval_pack_checks_smoke.json
  - when the canonical devstack is live, also updates:
      tools/eval_packs/basic_agentd_smoke.json
      tools/eval_packs/broker_smoke.json

Options:
  --state <path>    devstack_state.json path (default: out/devstack_state.json)
  --require-live    fail if live devstack-backed packs cannot be refreshed
  -h, --help        show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --state)
      STATE_PATH="$2"
      shift 2
      ;;
    --require-live)
      REQUIRE_LIVE=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      PACKS+=("$1")
      shift
      ;;
  esac
done

self_contained=(
  "${ROOT}/tools/eval_packs/eval_pack_smoke.json"
  "${ROOT}/tools/eval_packs/eval_pack_checks_smoke.json"
)
live_packs=(
  "${ROOT}/tools/eval_packs/basic_agentd_smoke.json"
  "${ROOT}/tools/eval_packs/broker_smoke.json"
)

if [[ ${#PACKS[@]} -eq 0 ]]; then
  PACKS=("${self_contained[@]}")
  if "${ROOT}/tools/devstack_status.sh" --state "${STATE_PATH}" --require-live >/dev/null 2>&1; then
    PACKS+=("${ROOT}/tools/eval_packs/basic_agentd_smoke.json")
    if "${ROOT}/tools/devstack_status.sh" --state "${STATE_PATH}" --require-ready >/dev/null 2>&1; then
      PACKS+=("${ROOT}/tools/eval_packs/broker_smoke.json")
    else
      echo "[eval-baselines] skipping broker_smoke; canonical devstack broker is not ready (${STATE_PATH})"
    fi
  elif [[ "${REQUIRE_LIVE}" == "1" ]]; then
    echo "live devstack required but unavailable: ${STATE_PATH}" >&2
    exit 1
  else
    echo "[eval-baselines] skipping live-stack packs; devstack not live (${STATE_PATH})"
  fi
fi

for pack in "${PACKS[@]}"; do
  pack_rel="$(python3 - "${ROOT}" "${pack}" <<'PY'
import os
import sys
print(os.path.relpath(sys.argv[2], sys.argv[1]))
PY
)"
  echo "[eval-baselines] update ${pack_rel}"
  AGENT_DEVSTACK_STATE="${STATE_PATH}" python3 "${ROOT}/tools/eval_pack.py" --file "${pack}" --update-baseline
done
