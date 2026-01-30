#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENT_BIN="${1:-}"
if [[ -z "${AGENT_BIN}" ]]; then
  echo "missing agent binary path arg" >&2
  exit 2
fi

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

PORT_STUB="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"
STUB_BASE="http://${HOST}:${PORT_STUB}/v1"

cleanup() {
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

# OpenAI-compatible streaming stub server:
# - requires stream=true; otherwise returns 400
# - first request: streams a tool call (delta.tool_calls) to fs_read README.md (fragmented args)
# - second request (after tool result): streams assistant content "OK"
python3 -u - <<PY > "${LOG_DIR}/agent_local_stream_tool_loop_smoke.stub.stdout.log" 2> "${LOG_DIR}/agent_local_stream_tool_loop_smoke.stub.stderr.log" &
import json
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

def has_tool_result(messages):
  for m in messages:
    if isinstance(m, dict) and m.get("role") == "tool":
      return True
  return False

def write_json(handler, status, obj):
  data = json.dumps(obj).encode("utf-8")
  handler.send_response(status)
  handler.send_header("Content-Type", "application/json; charset=utf-8")
  handler.send_header("Content-Length", str(len(data)))
  handler.end_headers()
  handler.wfile.write(data)

def write_sse(handler, chunks):
  handler.send_response(200)
  handler.send_header("Content-Type", "text/event-stream")
  handler.send_header("Cache-Control", "no-cache")
  handler.end_headers()
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

    if req.get("stream") is not True:
      write_json(self, 400, {"error": {"message": "expected stream=true"}})
      return

    messages = req.get("messages") if isinstance(req.get("messages"), list) else []

    if has_tool_result(messages):
      write_sse(self, [{"choices": [{"delta": {"content": "OK"}}]}])
      return

    write_sse(self, [
      {"choices": [{"delta": {"tool_calls": [
        {"index": 0, "id": "call_1", "type": "function", "function": {"name": "fs_read", "arguments": "{\"path\":\"README.md\","}}
      ]}}]},
      {"choices": [{"delta": {"tool_calls": [
        {"index": 0, "function": {"arguments": "\"max_lines\":5,\"max_chars\":20000}"}}
      ]}}]},
    ])

HTTPServer(("127.0.0.1", ${PORT_STUB}), H).serve_forever()
PY
STUB_PID=$!

out="$("${AGENT_BIN}" run "Read README.md then say OK" \
  --no-session \
  --tools host \
  --host-policy readonly \
  --base-url "${STUB_BASE}" \
  --api-key "dummy" \
  --model "stub" \
  --max-steps 4 \
  --stream-assistant \
  --quiet \
  2> "${LOG_DIR}/agent_local_stream_tool_loop_smoke.stderr.log")"

if [[ "${out}" != "OK" && "${out}" != "OK"$'\n' ]]; then
  echo "unexpected stdout:" >&2
  printf '%s\n' "${out}" >&2
  exit 1
fi

