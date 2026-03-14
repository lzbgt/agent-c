#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STATE_PATH="${AGENT_DEVSTACK_STATE:-${ROOT}/out/devstack_state.json}"
SET_NAME="self-contained"
BASELINE="auto"
UPDATE_BASELINE=0
PRINT_PLAN=0
REQUIRE_LIVE=0

usage() {
  cat <<'EOF'
Usage: tools/run_eval_pack_set.sh [--set <smoke|self-contained|canonical|live|all>] [--state <path>] [--baseline <path|auto>] [--update-baseline] [--require-live] [--print-plan]

Run a named canonical eval-pack set.

Sets:
  smoke            tools/eval_packs/eval_pack_smoke.json
  self-contained   smoke + eval_pack_checks_smoke
  canonical        self-contained + live-stack packs when the canonical devstack is live
  live             basic_agentd_smoke + broker_proxy_agentd_smoke, plus broker_smoke when broker is ready
  all              self-contained + live (requires live canonical devstack)

Behavior:
  - `canonical` skips live-stack packs when the canonical devstack is unavailable
  - `live` requires a live canonical devstack (`tools/devstack_status.sh --require-live`)
  - `--require-live` upgrades `canonical` to fail if the live devstack is unavailable
  - `broker_smoke` only runs when `tools/devstack_status.sh --require-ready` succeeds
  - `--baseline auto` maps each repo pack to ref/eval_packs/<pack>.summary.json
  - `--print-plan` prints one pack path per line and does not execute anything
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --set)
      SET_NAME="${2:-}"
      shift 2
      ;;
    --state)
      STATE_PATH="${2:-}"
      shift 2
      ;;
    --baseline)
      BASELINE="${2:-}"
      shift 2
      ;;
    --update-baseline)
      UPDATE_BASELINE=1
      shift
      ;;
    --print-plan)
      PRINT_PLAN=1
      shift
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
      echo "unknown arg: $1" >&2
      usage
      exit 2
      ;;
  esac
done

smoke_pack="${ROOT}/tools/eval_packs/eval_pack_smoke.json"
checks_pack="${ROOT}/tools/eval_packs/eval_pack_checks_smoke.json"
agentd_pack="${ROOT}/tools/eval_packs/basic_agentd_smoke.json"
broker_pack="${ROOT}/tools/eval_packs/broker_smoke.json"
broker_proxy_pack="${ROOT}/tools/eval_packs/broker_proxy_agentd_smoke.json"

packs=()

add_live_packs() {
  if ! "${ROOT}/tools/devstack_status.sh" --state "${STATE_PATH}" --require-live >/dev/null 2>&1; then
    if [[ "${REQUIRE_LIVE}" == "1" || "${SET_NAME}" == "live" || "${SET_NAME}" == "all" ]]; then
      echo "live devstack required for eval-pack set '${SET_NAME}': ${STATE_PATH}" >&2
      exit 1
    fi
    echo "[eval-pack-set] skipping live-stack packs; canonical devstack not live (${STATE_PATH})" >&2
    return 0
  fi
  packs+=("${agentd_pack}")
  packs+=("${broker_proxy_pack}")
  if "${ROOT}/tools/devstack_status.sh" --state "${STATE_PATH}" --require-ready >/dev/null 2>&1; then
    packs+=("${broker_pack}")
  else
    echo "[eval-pack-set] skipping broker_smoke; canonical devstack broker is not ready (${STATE_PATH})" >&2
  fi
}

case "${SET_NAME}" in
  smoke)
    packs=("${smoke_pack}")
    ;;
  self-contained)
    packs=("${smoke_pack}" "${checks_pack}")
    ;;
  canonical)
    packs=("${smoke_pack}" "${checks_pack}")
    add_live_packs
    ;;
  live)
    add_live_packs
    ;;
  all)
    packs=("${smoke_pack}" "${checks_pack}")
    add_live_packs
    ;;
  *)
    echo "unknown eval pack set: ${SET_NAME}" >&2
    usage
    exit 2
    ;;
esac

if [[ "${PRINT_PLAN}" == "1" ]]; then
  printf '%s\n' "${packs[@]}"
  exit 0
fi

if [[ "${BASELINE}" != "auto" && "${#packs[@]}" -gt 1 ]]; then
  echo "--baseline <path> only supports a single pack; use --baseline auto for sets" >&2
  exit 2
fi

for pack in "${packs[@]}"; do
  pack_rel="$(python3 - "${ROOT}" "${pack}" <<'PY'
import os
import sys
print(os.path.relpath(sys.argv[2], sys.argv[1]))
PY
)"
  echo "[eval-pack-set] run ${pack_rel}"
  cmd=(python3 "${ROOT}/tools/eval_pack.py" --file "${pack}")
  if [[ -n "${BASELINE}" ]]; then
    cmd+=(--baseline "${BASELINE}")
  fi
  if [[ "${UPDATE_BASELINE}" == "1" ]]; then
    cmd+=(--update-baseline)
  fi
  AGENT_DEVSTACK_STATE="${STATE_PATH}" "${cmd[@]}"
done
