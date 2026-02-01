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

SESSION_ID="agentd_ui_wait_any_all_smoke_$(date +%s)_$RANDOM"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

# OpenAI-compatible stub:
# - 0 tool results: call client_wait_any(predicates=[A,B])
# - 1 tool result: call client_wait_all(predicates=[C,D])
# - 2+ tool results: assistant "OK"
python3 -u - <<PY > "${LOG_DIR}/agentd_ui_wait_any_all_smoke.stub.stdout.log" 2> "${LOG_DIR}/agentd_ui_wait_any_all_smoke.stub.stderr.log" &
import json
from http.server import BaseHTTPRequestHandler, HTTPServer

def tool_result_count(messages):
  n = 0
  for m in messages:
    if isinstance(m, dict) and m.get("role") == "tool":
      n += 1
  return n

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
    tr = tool_result_count(messages)

    if tr >= 2:
      body = {
        "id": "cmpl_stub_3",
        "object": "chat.completion",
        "created": 0,
        "model": "stub",
        "choices": [
          {"index": 0, "message": {"role": "assistant", "content": "OK"}, "finish_reason": "stop"}
        ],
      }
    elif tr == 1:
      body = {
        "id": "cmpl_stub_2",
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
                  "id": "call_wait_all_1",
                  "type": "function",
                  "function": {
                    "name": "client_wait_all",
                    "arguments": json.dumps({
                      "timeout_ms": 8000,
                      "predicates": [
                        {"type":"smoke_c","data_match":{"k":"c"}},
                        {"type":"smoke_d","data_match":{"k":"d"}},
                      ],
                    }),
                  },
                }
              ],
            },
            "finish_reason": "tool_calls",
          }
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
                  "id": "call_wait_any_1",
                  "type": "function",
                  "function": {
                    "name": "client_wait_any",
                    "arguments": json.dumps({
                      "timeout_ms": 8000,
                      "predicates": [
                        {"type":"smoke_a","data_match":{"k":"a"}},
                        {"type":"smoke_b","data_match":{"k":"b"}},
                      ],
                    }),
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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_ui_wait_any_all_smoke" \
  --tools host \
  --no-yolo

agentd_smoke_wait_health "${DAEMON_URL}"

# Post the join events in phases so ui_wait_any triggers first, then ui_wait_all.
(
  sleep 0.8
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/json" \
    -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${SESSION_ID}",
  "type": "smoke_b",
  "data": {"k":"b"},
  "append_to_session": False
}))
PY
)" \
    "${DAEMON_URL}/api/v1/session/client_event" >/dev/null

  sleep 0.8
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/json" \
    -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${SESSION_ID}",
  "type": "smoke_c",
  "data": {"k":"c"},
  "append_to_session": False
}))
PY
)" \
    "${DAEMON_URL}/api/v1/session/client_event" >/dev/null

  sleep 0.2
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/json" \
    -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${SESSION_ID}",
  "type": "smoke_d",
  "data": {"k":"d"},
  "append_to_session": False
}))
PY
)" \
    "${DAEMON_URL}/api/v1/session/client_event" >/dev/null
) &

resp="$(curl -fsS --noproxy "*" --max-time 25 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "wait for join events then say OK",
  "session_id": "${SESSION_ID}",
  "tools": "host",
  "yolo": False,
  "base_url": "${STUB_BASE}",
  "api_key": "dummy",
  "model": "stub",
  "max_steps": 6,
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
ev = obj.get("events") or []
def has_tool_result(name):
  for e in ev:
    if isinstance(e, dict) and e.get("type") == "tool_result":
      d = e.get("data") if isinstance(e.get("data"), dict) else {}
      if d.get("tool_name") == name:
        return True
  return False
if not has_tool_result("client_wait_any"):
  print("expected tool_result for client_wait_any; got:", ev, file=sys.stderr)
  raise SystemExit(1)
if not has_tool_result("client_wait_all"):
  print("expected tool_result for client_wait_all; got:", ev, file=sys.stderr)
  raise SystemExit(1)
txt = (obj.get("assistant_text") or "").strip()
if txt != "OK":
  print("unexpected assistant_text:", txt, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_ui_wait_any_all_smoke OK"
