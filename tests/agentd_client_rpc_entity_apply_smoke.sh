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

SESSION_ID="agentd_client_rpc_entity_apply_smoke_$(date +%s)_$RANDOM"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

# OpenAI-compatible stub:
# - 0 tool results: call ui_action(type=client_rpc, rpc.kind=entity_apply)
# - 1 tool result: call client_wait_event(type=client_rpc_result, data_match.rpc_id=r1)
# - 2+ tool results: assistant "OK"
python3 -u - <<PY > "${LOG_DIR}/agentd_client_rpc_entity_apply_smoke.stub.stdout.log" 2> "${LOG_DIR}/agentd_client_rpc_entity_apply_smoke.stub.stderr.log" &
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
                  "id": "call_wait_rpc_result_1",
                  "type": "function",
                  "function": {
                    "name": "client_wait_event",
                    "arguments": json.dumps({"type":"client_rpc_result","timeout_ms":5000,"data_match":{"rpc_id":"r1"}}),
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
                  "id": "call_ui_action_rpc_1",
                  "type": "function",
                  "function": {
                    "name": "ui_action",
                    "arguments": json.dumps({
                      "type": "client_rpc",
                      "title": "create a scene entity",
                      "rpc_id": "r1",
                      "rpc": {"kind": "entity_apply", "side_effects": True, "args": {"ops":[{"op":"create","id":"plot-1","entity_kind":"canvas2d","props":{"width":320,"height":120}}]}},
                      "auto_run": True,
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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_client_rpc_entity_apply_smoke" \
  --tools host \
  --no-yolo

agentd_smoke_wait_health "${DAEMON_URL}"

(
  sleep 1.0
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/json" \
    -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${SESSION_ID}",
  "type": "client_rpc_result",
  "client": {"id":"smoke-client","kind":"test","instance_id":"one"},
  "data": {"rpc_id":"r1","ok": True, "rpc_kind":"entity_apply", "result":{"ok": True}},
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
  "prompt": "create an entity via client rpc, wait for result, then say OK",
  "session_id": "${SESSION_ID}",
  "tools": "host",
  "yolo": False,
  "tools_root": "@host",
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

def has_ui_action_entity_apply():
  for e in ev:
    if isinstance(e, dict) and e.get("type") == "ui_action":
      d = e.get("data") if isinstance(e.get("data"), dict) else {}
      a = d.get("action") if isinstance(d.get("action"), dict) else {}
      if a.get("type") != "client_rpc":
        continue
      rpc = a.get("rpc") if isinstance(a.get("rpc"), dict) else {}
      if rpc.get("kind") == "entity_apply":
        return True
  return False

if not has_ui_action_entity_apply():
  print("expected ui_action client_rpc entity_apply; got:", ev, file=sys.stderr)
  raise SystemExit(1)

txt = (obj.get("assistant_text") or "").strip()
if txt != "OK":
  print("unexpected assistant_text:", txt, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_client_rpc_entity_apply_smoke OK"

