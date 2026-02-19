#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

SCENARIO_PATH="${TMP_DIR}/scenario.json"
PACK_PATH="${TMP_DIR}/pack.json"
OUT_DIR="${TMP_DIR}/out"

cat > "${SCENARIO_PATH}" <<'JSON'
{
  "name": "eval_pack_smoke",
  "steps": [
    {
      "type": "shell",
      "name": "write_marker",
      "cmd": "echo hello > {{run_dir}}/hello.txt"
    }
  ]
}
JSON

cat > "${PACK_PATH}" <<JSON
{
  "name": "eval_pack_smoke",
  "version": "eval_pack_v0",
  "threshold": { "min_score": 1.0, "min_pass_rate": 1.0 },
  "scenarios": [
    {
      "id": "smoke",
      "file": "${SCENARIO_PATH}",
      "score_weight": 1.0,
      "checks": [
        { "type": "file_exists", "path": "hello.txt" },
        { "type": "json_path", "path": "meta.json", "key": "scenario", "equals": "eval_pack_smoke" }
      ]
    }
  ]
}
JSON

python3 "${ROOT}/tools/eval_pack.py" --file "${PACK_PATH}" --out-dir "${OUT_DIR}" >/dev/null
