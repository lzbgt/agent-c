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

DB_PATH="${LOG_DIR}/agentd_db_query_smoke.sqlite"
SESSION_ID="agentd_db_query_smoke_$(date +%s)_$RANDOM"

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
python3 -u - <<PY > "${LOG_DIR}/agentd_db_query_smoke.stub.stdout.log" 2> "${LOG_DIR}/agentd_db_query_smoke.stub.stderr.log" &
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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_db_query_smoke" \
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

db_runs="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/runs?session_id=$(python3 -c 'import urllib.parse; print(urllib.parse.quote("""'${SESSION_ID}'"""))')&limit=20&offset=0")"

RUN_ID="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${db_runs}''')
if not obj.get("ok"):
  print("db/runs failed:", obj, file=sys.stderr)
  raise SystemExit(1)
runs = obj.get("runs") or []
if not runs:
  print("expected at least 1 run row", file=sys.stderr)
  raise SystemExit(1)
rid = runs[0].get("run_id")
if not isinstance(rid, int) or rid <= 0:
  print("invalid run_id:", rid, file=sys.stderr)
  raise SystemExit(1)
print(rid)
PY
)"

db_run="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/run?run_id=${RUN_ID}&include_events=1&include_tools=1&include_artifacts=1")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${db_run}''')
if not obj.get("ok"):
  print("db/run failed:", obj, file=sys.stderr)
  raise SystemExit(1)
run = obj.get("run") or {}
if int(run.get("run_id") or 0) != int(${RUN_ID}):
  print("unexpected run_id:", run.get("run_id"), file=sys.stderr)
  raise SystemExit(1)
tools = obj.get("tool_records") or []
if len(tools) < 1:
  print("expected tool_records >= 1", file=sys.stderr)
  raise SystemExit(1)
events = obj.get("events") or []
if not isinstance(events, list) or len(events) < 1:
  print("expected events >= 1", file=sys.stderr)
  raise SystemExit(1)
# If server parsed data_json successfully, it should expose `data` for at least some rows.
has_parsed = any(isinstance(e, dict) and isinstance(e.get("data"), dict) for e in events)
if not has_parsed:
  print("expected at least one parsed event.data dict", file=sys.stderr)
  raise SystemExit(1)
PY

db_arts="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/artifacts?session_id=$(python3 -c 'import urllib.parse; print(urllib.parse.quote("""'${SESSION_ID}'"""))')&limit=20&offset=0")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${db_arts}''')
if not obj.get("ok"):
  print("db/artifacts failed:", obj, file=sys.stderr)
  raise SystemExit(1)
PY
