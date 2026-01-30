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

PORT_DAEMON="$(agentd_smoke_pick_port)"
PORT_STUB="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"
STUB_BASE="http://${HOST}:${PORT_STUB}/v1"

DB_PATH="${LOG_DIR}/agentd_db_smoke.sqlite"
SESSION_ID="agentd_db_smoke_$(date +%s)_$RANDOM"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
  rm -f "${DB_PATH}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

# OpenAI-compatible stub:
# - first response: tool call to fs_read README.md
# - second response (after tool result): assistant "OK"
python3 -u - <<PY > "${LOG_DIR}/agentd_db_smoke.stub.stdout.log" 2> "${LOG_DIR}/agentd_db_smoke.stub.stderr.log" &
import json
from http.server import BaseHTTPRequestHandler, HTTPServer

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
        "id": "cmpl_stub_2",
        "object": "chat.completion",
        "created": 0,
        "model": "stub",
        "choices": [
          {"index": 0, "message": {"role": "assistant", "content": "OK"}, "finish_reason": "stop"}
        ],
      }
    else:
      body = {
        "id": "cmpl_stub_1",
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
                  "id": "call_1",
                  "type": "function",
                  "function": {
                    "name": "fs_read",
                    "arguments": json.dumps({"path": "README.md", "max_lines": 10, "max_chars": 20000}),
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

HTTPServer(("127.0.0.1", ${PORT_STUB}), H).serve_forever()
PY
STUB_PID=$!

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_db_smoke" \
  --tools host \
  --no-yolo \
  --db-path "${DB_PATH}"

agentd_smoke_wait_health "${DAEMON_URL}"

resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "Read README.md then say OK",
  "session_id": "${SESSION_ID}",
  "tools": "host",
  "yolo": False,
  "tools_root": "@host",
  "base_url": "${STUB_BASE}",
  "api_key": "dummy",
  "model": "stub",
  "max_steps": 4,
  "verbose": True
}))
PY
)" \
  "${DAEMON_URL}/api/v1/run")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp}''')
if not obj.get("ok"):
  print("run failed:", obj, file=sys.stderr)
  raise SystemExit(1)
txt = (obj.get("assistant_text") or "").strip()
if txt != "OK":
  print("unexpected assistant_text:", txt, file=sys.stderr)
  raise SystemExit(1)
PY

python3 - <<PY
import sqlite3, sys, os
path = r'''${DB_PATH}'''
if not os.path.exists(path):
  print("db file missing:", path, file=sys.stderr)
  raise SystemExit(1)
db = sqlite3.connect(path)
cur = db.cursor()
cur.execute("SELECT COUNT(*) FROM sessions WHERE session_id=?", (r'''${SESSION_ID}''',))
sessions = cur.fetchone()[0]
cur.execute("SELECT COUNT(*) FROM messages WHERE session_id=?", (r'''${SESSION_ID}''',))
messages = cur.fetchone()[0]
cur.execute("SELECT COUNT(*) FROM runs WHERE session_id=?", (r'''${SESSION_ID}''',))
runs = cur.fetchone()[0]
cur.execute("SELECT COUNT(*) FROM tool_records")
tools = cur.fetchone()[0]
cur.execute("SELECT value FROM meta WHERE key='schema_version' LIMIT 1")
schema_version_raw = cur.fetchone()
schema_version = int(schema_version_raw[0]) if schema_version_raw and schema_version_raw[0] else 0
cur.execute("SELECT COUNT(*) FROM artifacts")
artifacts = cur.fetchone()[0]
db.close()
if sessions != 1:
  print("expected sessions=1 got", sessions, file=sys.stderr)
  raise SystemExit(1)
if messages < 2:
  print("expected messages>=2 got", messages, file=sys.stderr)
  raise SystemExit(1)
if runs != 1:
  print("expected runs=1 got", runs, file=sys.stderr)
  raise SystemExit(1)
if tools < 1:
  print("expected tool_records>=1 got", tools, file=sys.stderr)
  raise SystemExit(1)
if schema_version < 4:
  print("expected schema_version>=4 got", schema_version, file=sys.stderr)
  raise SystemExit(1)
if artifacts < 0:
  print("expected artifacts>=0 got", artifacts, file=sys.stderr)
  raise SystemExit(1)
PY
