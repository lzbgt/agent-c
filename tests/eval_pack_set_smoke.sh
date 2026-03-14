#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

smoke_plan="${TMP_DIR}/smoke.plan"
self_contained_plan="${TMP_DIR}/self_contained.plan"
canonical_plan="${TMP_DIR}/canonical.plan"

"${ROOT}/tools/run_eval_pack_set.sh" --set smoke --print-plan >"${smoke_plan}"
"${ROOT}/tools/run_eval_pack_set.sh" --set self-contained --print-plan >"${self_contained_plan}"
"${ROOT}/tools/run_eval_pack_set.sh" --set canonical --state "${TMP_DIR}/missing_state.json" --print-plan >"${canonical_plan}"

python3 - <<PY
from pathlib import Path

smoke = Path("${smoke_plan}").read_text(encoding="utf-8").strip().splitlines()
self_contained = Path("${self_contained_plan}").read_text(encoding="utf-8").strip().splitlines()
canonical = Path("${canonical_plan}").read_text(encoding="utf-8").strip().splitlines()

assert smoke == ["${ROOT}/tools/eval_packs/eval_pack_smoke.json"], smoke
assert self_contained == [
    "${ROOT}/tools/eval_packs/eval_pack_smoke.json",
    "${ROOT}/tools/eval_packs/eval_pack_checks_smoke.json",
], self_contained
assert canonical == self_contained, canonical
PY

if "${ROOT}/tools/run_eval_pack_set.sh" --set live --state "${TMP_DIR}/missing_state.json" --print-plan >/dev/null 2>&1; then
  echo "expected live eval-pack set to fail when devstack state is unavailable" >&2
  exit 1
fi

if "${ROOT}/tools/run_eval_pack_set.sh" --set canonical --state "${TMP_DIR}/missing_state.json" --require-live --print-plan >/dev/null 2>&1; then
  echo "expected canonical eval-pack set with --require-live to fail when devstack state is unavailable" >&2
  exit 1
fi
