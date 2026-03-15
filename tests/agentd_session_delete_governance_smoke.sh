#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "usage: agentd_session_delete_governance_smoke.sh <agentd_bin>" >&2
  exit 2
fi

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

PORT_DAEMON="$(agentd_smoke_pick_port)"
PORT_STUB="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"
STUB_BASE="http://${HOST}:${PORT_STUB}/v1"
NAME="agentd_session_delete_governance_smoke"
DB_PATH="${LOG_DIR}/${NAME}.sqlite"
SESSION_ID="${NAME}_$(date +%s)_$RANDOM"
PROJECT_ROOT="$(agentd_smoke_project_root)"
README_PATH="${PROJECT_ROOT}/README.md"
export README_PATH
export AGENTD_RUN_ATTEST_HMAC_KID="delete_gov_kid"
export AGENTD_RUN_ATTEST_HMAC_KEY="delete_gov_secret"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
  rm -f "${DB_PATH}" "${DB_PATH}-wal" "${DB_PATH}-shm" >/dev/null 2>&1 || true
}
trap cleanup EXIT

python3 -u - <<PY > "${LOG_DIR}/${NAME}.stub.stdout.log" 2> "${LOG_DIR}/${NAME}.stub.stderr.log" &
import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

README_PATH = os.environ.get("README_PATH", "README.md")

def has_tool_result(messages):
  for m in messages:
    if isinstance(m, dict) and m.get("role") == "tool":
      return True
  return False

class H(BaseHTTPRequestHandler):
  def log_message(self, fmt, *args):
    return

  def do_POST(self):
    if self.path != "/v1/chat/completions":
      self.send_response(404)
      self.end_headers()
      return
    length = int(self.headers.get("content-length") or "0")
    raw = self.rfile.read(length) if length > 0 else b"{}"
    try:
      req = json.loads(raw.decode("utf-8"))
    except Exception:
      req = {}
    messages = req.get("messages") if isinstance(req.get("messages"), list) else []

    if has_tool_result(messages):
      body = {
        "id": "cmpl_delete_gov_2",
        "object": "chat.completion",
        "created": 0,
        "model": "stub",
        "choices": [
          {"index": 0, "message": {"role": "assistant", "content": "OK"}, "finish_reason": "stop"}
        ],
      }
    else:
      body = {
        "id": "cmpl_delete_gov_1",
        "object": "chat.completion",
        "created": 0,
        "model": "stub",
        "choices": [
          {
            "index": 0,
            "message": {
              "role": "assistant",
              "content": "",
              "tool_calls": [
                {
                  "id": "call_delete_gov_1",
                  "type": "function",
                  "function": {
                    "name": "fs_read",
                    "arguments": json.dumps({"path": README_PATH, "max_lines": 5, "max_chars": 20000}),
                  },
                }
              ],
            },
            "finish_reason": "tool_calls",
          }
        ],
      }

    data = json.dumps(body).encode("utf-8")
    self.send_response(200)
    self.send_header("Content-Type", "application/json; charset=utf-8")
    self.send_header("Content-Length", str(len(data)))
    self.end_headers()
    self.wfile.write(data)

ThreadingHTTPServer(("127.0.0.1", ${PORT_STUB}), H).serve_forever()
PY
STUB_PID=$!

rm -f "${DB_PATH}" "${DB_PATH}-wal" "${DB_PATH}-shm" >/dev/null 2>&1 || true

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}" \
  --tools host \
  --no-yolo \
  --db-path "${DB_PATH}"

agentd_smoke_wait_health "${DAEMON_URL}"

run_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "read the first 5 lines of README.md and say OK",
  "session_id": "${SESSION_ID}",
  "tools": "host",
  "base_url": "${STUB_BASE}",
  "model": "stub",
  "api_key": "secret_should_not_persist"
}))
PY
)" \
  "${DAEMON_URL}/api/v1/run")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${run_resp}''')
if not obj.get("ok"):
  print("run failed", obj, file=sys.stderr)
  raise SystemExit(1)
PY

SESSION_ID_Q="$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote("${SESSION_ID}"))
PY
)"

runs_before="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/runs?session_id=${SESSION_ID_Q}")"

run_id="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${runs_before}''')
if not obj.get("ok"):
  print("db runs failed", obj, file=sys.stderr)
  raise SystemExit(1)
runs = obj.get("runs") or []
if len(runs) != 1:
  print("expected exactly one run before delete", runs, file=sys.stderr)
  raise SystemExit(1)
run_id = runs[0].get("run_id")
if not isinstance(run_id, int) or run_id <= 0:
  print("missing run_id", runs[0], file=sys.stderr)
  raise SystemExit(1)
print(run_id)
PY
)"

replay_before="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/run/replay?run_id=${run_id}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${replay_before}''')
if not obj.get("ok"):
  print("replay before delete failed", obj, file=sys.stderr)
  raise SystemExit(1)
bundle = obj.get("bundle") or {}
if bundle.get("schema") != "run_replay_bundle_v1":
  print("unexpected replay schema", bundle.get("schema"), file=sys.stderr)
  raise SystemExit(1)
req = bundle.get("request") or {}
if "api_key" in req:
  print("api_key should be redacted before delete", req, file=sys.stderr)
  raise SystemExit(1)
PY

delete_resp="$(curl -fsS --noproxy "*" --max-time 10 -X DELETE \
  "${DAEMON_URL}/api/v1/session?session_id=${SESSION_ID_Q}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${delete_resp}''')
if not obj.get("ok"):
  print("session delete failed", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("session_id") != "${SESSION_ID}":
  print("unexpected session_id", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("deleted_from_db") is not True:
  print("expected deleted_from_db=true", obj, file=sys.stderr)
  raise SystemExit(1)
PY

session_after_body="${LOG_DIR}/${NAME}.session_after_delete.json"
session_after_status="$(curl -sS --noproxy "*" --max-time 10 \
  -o "${session_after_body}" -w "%{http_code}" \
  "${DAEMON_URL}/api/v1/session?session_id=${SESSION_ID_Q}")"

python3 - <<PY
import json, sys
status = int(r'''${session_after_status}''')
with open(r'''${session_after_body}''', 'r', encoding='utf-8') as fh:
  body = json.load(fh)
if status != 404:
  print("expected session 404 after delete", status, body, file=sys.stderr)
  raise SystemExit(1)
if body.get("error") != "session not found":
  print("unexpected session delete body", body, file=sys.stderr)
  raise SystemExit(1)
PY

runs_after="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/runs?session_id=${SESSION_ID_Q}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${runs_after}''')
if not obj.get("ok"):
  print("db runs after delete failed", obj, file=sys.stderr)
  raise SystemExit(1)
if (obj.get("count") or 0) != 0:
  print("expected zero runs after delete", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("runs") not in ([], None):
  print("expected empty runs after delete", obj, file=sys.stderr)
  raise SystemExit(1)
PY

replay_after_body="${LOG_DIR}/${NAME}.replay_after_delete.json"
replay_after_status="$(curl -sS --noproxy "*" --max-time 10 \
  -o "${replay_after_body}" -w "%{http_code}" \
  "${DAEMON_URL}/api/v1/run/replay?run_id=${run_id}")"

python3 - <<PY
import json, sys
status = int(r'''${replay_after_status}''')
with open(r'''${replay_after_body}''', 'r', encoding='utf-8') as fh:
  body = json.load(fh)
if status != 404:
  print("expected replay 404 after delete", status, body, file=sys.stderr)
  raise SystemExit(1)
if body.get("error") != "run not found":
  print("unexpected replay-after-delete body", body, file=sys.stderr)
  raise SystemExit(1)
PY

att_after_body="${LOG_DIR}/${NAME}.attestation_after_delete.json"
att_after_status="$(curl -sS --noproxy "*" --max-time 10 \
  -o "${att_after_body}" -w "%{http_code}" \
  "${DAEMON_URL}/api/v1/run/attestation?run_id=${run_id}")"

python3 - <<PY
import json, sys
status = int(r'''${att_after_status}''')
with open(r'''${att_after_body}''', 'r', encoding='utf-8') as fh:
  body = json.load(fh)
if status != 404:
  print("expected attestation 404 after delete", status, body, file=sys.stderr)
  raise SystemExit(1)
if body.get("error") != "run not found":
  print("unexpected attestation-after-delete body", body, file=sys.stderr)
  raise SystemExit(1)
PY

echo "ok: ${NAME}"
