#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
python3 "${ROOT}/tools/eval_pack.py" --file "${ROOT}/tools/eval_packs/eval_pack_checks_smoke.json" --baseline auto >/dev/null
