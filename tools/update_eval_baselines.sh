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
  - updates the canonical eval-pack set:
      self-contained packs always
      live-stack packs when the canonical devstack is live

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

if [[ ${#PACKS[@]} -eq 0 ]]; then
  args=(--set canonical --state "${STATE_PATH}" --update-baseline)
  if [[ "${REQUIRE_LIVE}" == "1" ]]; then
    args+=(--require-live)
  fi
  exec "${ROOT}/tools/run_eval_pack_set.sh" "${args[@]}"
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
