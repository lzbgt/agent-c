#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

SCENARIO_PATH="${TMP_DIR}/scenario.json"
PACK_PATH="${TMP_DIR}/pack.json"
OUT_DIR="${TMP_DIR}/out"
OUT_DIR_2="${TMP_DIR}/out_2"
BASELINE_PATH="${TMP_DIR}/baseline.json"

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

python3 "${ROOT}/tools/eval_pack.py" --file "${PACK_PATH}" --out-dir "${OUT_DIR}" --baseline "${BASELINE_PATH}" --update-baseline >/dev/null
python3 "${ROOT}/tools/eval_pack.py" --file "${PACK_PATH}" --out-dir "${OUT_DIR_2}" --baseline "${BASELINE_PATH}" >/dev/null

python3 - <<PY
import json

with open("${BASELINE_PATH}", "r", encoding="utf-8") as f:
    payload = json.load(f)

assert payload["baseline_version"] == "eval_pack_baseline_v1"
assert payload["name"] == "eval_pack_smoke"
assert payload["ok"] is True
assert payload["total_score"] == 1.0
assert "started_at" not in payload
assert "finished_at" not in payload
assert isinstance(payload.get("results"), list) and len(payload["results"]) == 1
row = payload["results"][0]
assert row["id"] == "smoke"
assert row["ok"] is True
assert row["score"] == 1.0
assert "file" not in row
assert "run_dir" not in row
PY
