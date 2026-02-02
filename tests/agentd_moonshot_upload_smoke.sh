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

PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

source "${SCRIPT_DIR}/test_keys.sh"
agent_test_setup_proxy_env
MOONSHOT_KEY="$(agent_test_get_key moonshot 2>/dev/null || true)"
if [[ -z "${MOONSHOT_KEY}" ]]; then
  echo "SKIP: KIMI_API_KEY_CN (or MOONSHOT_API_KEY) not set and not found in .not_in_repo/project.local.md" >&2
  exit 77
fi

BASE_URL="${MOONSHOT_API_BASE:-https://api.moonshot.cn/v1}"
MODEL="${AGENT_TEST_MOONSHOT_MODEL:-kimi-k2.5}"

HOST="127.0.0.1"
PORT="$(agentd_smoke_pick_port)"

SESSION_ID="agentd_moonshot_upload_$(date +%s)_$RANDOM"

trap agentd_smoke_stop EXIT

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

KIMI_API_KEY_CN="${MOONSHOT_KEY}" agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "agentd_moonshot_upload" \
  --base-url "${BASE_URL}" \
  --model "${MODEL}" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

upload_body="$(
  # Self-contained: tiny 1x1 PNG (base64). Avoid relying on a binary fixture file in the repo.
  IMG_B64="iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==" \
  SESSION_ID="${SESSION_ID}" python3 - <<PY
import json, os
print(json.dumps({
  "session_id": os.environ["SESSION_ID"],
  "files": [{
    "name": "image.png",
    "mime": "image/png",
    "data_base64": os.environ["IMG_B64"]
  }]
}))
PY
)"

upload_resp="$(
  printf '%s' "${upload_body}" | curl -fsS --noproxy "*" --max-time 120 \
    -H "Content-Type: application/json" \
    -d @- \
    "${DAEMON_URL}/api/v1/session/upload"
)"

uploaded_path="$(
  UPLOAD_RESP="${upload_resp}" python3 - <<PY
import json, os, sys
obj = json.loads(os.environ["UPLOAD_RESP"])
if not obj.get("ok"):
  raise SystemExit(1)
files = obj.get("files") or []
if not files or not isinstance(files, list):
  raise SystemExit(1)
p = files[0].get("path")
if not isinstance(p, str) or not p:
  raise SystemExit(1)
print(p)
PY
)"

run_body="$(
  SESSION_ID="${SESSION_ID}" UPLOADED_PATH="${uploaded_path}" python3 - <<PY
import json, os
print(json.dumps({
  "prompt": "Describe the image briefly in one sentence.",
  "session_id": os.environ["SESSION_ID"],
  "tools": "none",
  "input_files": [{"path": os.environ["UPLOADED_PATH"], "mime": "image/png", "kind": "image"}],
  "max_chars": 20000,
  "keep_last": 16,
  "verbose": False
}))
PY
)"

run_resp="$(
  printf '%s' "${run_body}" | curl -fsS --noproxy "*" --max-time 240 \
    -H "Content-Type: application/json" \
    -d @- \
    "${DAEMON_URL}/api/v1/run"
)"

RUN_RESP="${run_resp}" python3 - <<PY
import json, os, sys
obj = json.loads(os.environ["RUN_RESP"])
if not obj.get("ok"):
  print("daemon run failed", obj, file=sys.stderr)
  raise SystemExit(1)
txt = (obj.get("assistant_text") or "").strip()
if not txt:
  print("missing assistant_text", file=sys.stderr)
  raise SystemExit(1)
PY

# Cleanup session artifacts.
curl -fsS --noproxy "*" -X DELETE "${DAEMON_URL}/api/v1/session?session_id=$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote("${SESSION_ID}"))
PY
)" --max-time 10 >/dev/null || true
