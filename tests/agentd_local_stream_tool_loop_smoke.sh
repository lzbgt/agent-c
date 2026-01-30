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

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

# Start an OpenAI-compatible streaming stub server that:
# - step 0: streams a tool call (delta.tool_calls) for fs_read README.md (arguments are fragmented)
# - step 1: streams assistant content "OK"
python3 -u - <<PY > "${LOG_DIR}/agentd_local_stream_tool_loop_smoke.stub.stdout.log" 2> "${LOG_DIR}/agentd_local_stream_tool_loop_smoke.stub.stderr.log" &
import json
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

def has_tool_result(messages):
  for m in messages:
    if isinstance(m, dict) and m.get("role") == "tool":
      return True
  return False

def write_sse(handler, chunks):
  for c in chunks:
    handler.wfile.write(("data: " + json.dumps(c) + "\n\n").encode("utf-8"))
    handler.wfile.flush()
    time.sleep(0.02)
  handler.wfile.write(b"data: [DONE]\n\n")
  handler.wfile.flush()

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
    stream = bool(req.get("stream"))

    if not stream:
      # Allow non-streaming callers too, but this test expects streaming.
      body = {
        "id": "cmpl_nonstream",
        "object": "chat.completion",
        "created": 0,
        "model": "stub",
        "choices": [
          {"index": 0, "message": {"role": "assistant", "content": "OK"}, "finish_reason": "stop"}
        ],
      }
      data = json.dumps(body).encode("utf-8")
      self.send_response(200)
      self.send_header("Content-Type", "application/json; charset=utf-8")
      self.send_header("Content-Length", str(len(data)))
      self.end_headers()
      self.wfile.write(data)
      return

    self.send_response(200)
    self.send_header("Content-Type", "text/event-stream")
    self.send_header("Cache-Control", "no-cache")
    self.end_headers()

    if has_tool_result(messages):
      chunks = [
        {"choices": [{"delta": {"content": "OK"}}]},
      ]
      write_sse(self, chunks)
      return

    # Stream a tool call with fragmented arguments.
    chunks = [
      {"choices": [{"delta": {"tool_calls": [
        {"index": 0, "id": "call_1", "type": "function", "function": {"name": "fs_read", "arguments": "{\"path\":\"README.md\","}}
      ]}}]},
      {"choices": [{"delta": {"tool_calls": [
        {"index": 0, "function": {"arguments": "\"max_lines\":5,\"max_chars\":20000}"}}
      ]}}]},
    ]
    write_sse(self, chunks)

HTTPServer(("127.0.0.1", ${PORT_STUB}), H).serve_forever()
PY
STUB_PID=$!

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_local_stream_tool_loop_smoke" \
  --tools host \
  --no-yolo

agentd_smoke_wait_health "${DAEMON_URL}"

resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "Read README.md then say OK",
  "no_session": True,
  "tools": "host",
  "yolo": False,
  "tools_root": "@host",
  "base_url": "${STUB_BASE}",
  "api_key": "dummy",
  "model": "stub",
  "max_steps": 4,
  "stream_assistant": True,
  "verbose": False,
  "trace": False
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
events = obj.get("events") or []
if not isinstance(events, list):
  print("missing events array", obj, file=sys.stderr)
  raise SystemExit(1)
types = [e.get("type") for e in events if isinstance(e, dict)]
if "tool_call" not in types:
  print("missing tool_call event types=", types, file=sys.stderr)
  raise SystemExit(1)
if "tool_result" not in types:
  print("missing tool_result event types=", types, file=sys.stderr)
  raise SystemExit(1)
if "assistant_delta" not in types:
  print("expected assistant_delta in tool loop events", types, file=sys.stderr)
  raise SystemExit(1)
PY

