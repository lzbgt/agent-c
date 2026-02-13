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

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

PORT="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "agentd_upload_limit_smoke" \
  --upload-max-bytes 16

agentd_smoke_wait_health "${DAEMON_URL}"

sid="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "{}" \
  "${DAEMON_URL}/api/v1/session/new" | python3 -c 'import json,sys; obj=json.load(sys.stdin); assert obj.get("ok") and obj.get("session_id"); print(obj["session_id"])'
)"

resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import base64, json
sid = "${sid}"
small = base64.b64encode(b"hello").decode("ascii")
large = base64.b64encode(b"a" * 32).decode("ascii")
print(json.dumps({
  "session_id": sid,
  "files": [
    {"name": "ok.txt", "mime": "text/plain", "data_base64": small},
    {"name": "big.bin", "mime": "application/octet-stream", "data_base64": large},
  ]
}))
PY
)" \
  "${DAEMON_URL}/api/v1/session/upload")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp}''')
if not obj.get("ok"):
  print("expected ok response", obj, file=sys.stderr)
  raise SystemExit(1)
files = obj.get("files") or []
errs = obj.get("errors") or []
if len(files) != 1:
  print("expected 1 accepted file, got", files, file=sys.stderr)
  raise SystemExit(1)
if not errs:
  print("expected errors for oversized file", obj, file=sys.stderr)
  raise SystemExit(1)
codes = [e.get("code") for e in errs if isinstance(e, dict)]
if "file_too_large" not in codes:
  print("missing file_too_large in errors", errs, file=sys.stderr)
  raise SystemExit(1)
PY

tmp_resp="${LOG_DIR}/agentd_upload_limit_smoke.second.json"
status="$(curl -sS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import base64, json
sid = "${sid}"
large = base64.b64encode(b"a" * 64).decode("ascii")
print(json.dumps({
  "session_id": sid,
  "files": [{"name": "too_big.bin", "data_base64": large}]
}))
PY
)" \
  -o "${tmp_resp}" -w "%{http_code}" \
  "${DAEMON_URL}/api/v1/session/upload")"

if [[ "${status}" != "400" ]]; then
  echo "expected HTTP 400, got ${status}" >&2
  cat "${tmp_resp}" >&2 || true
  exit 1
fi

python3 - <<PY
import json, sys
obj = json.load(open("${tmp_resp}", "r", encoding="utf-8"))
if obj.get("ok"):
  print("expected ok=false for oversized-only upload", obj, file=sys.stderr)
  raise SystemExit(1)
errs = obj.get("errors") or []
codes = [e.get("code") for e in errs if isinstance(e, dict)]
if "file_too_large" not in codes:
  print("missing file_too_large in errors", errs, file=sys.stderr)
  raise SystemExit(1)
PY
