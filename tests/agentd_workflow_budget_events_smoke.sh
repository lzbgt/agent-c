#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
PLUGIN_PATH="${2:-}"
if [[ -z "${AGENTD_BIN}" || -z "${PLUGIN_PATH}" ]]; then
  echo "usage: agentd_workflow_budget_events_smoke.sh <agentd_bin> <plugin_path>" >&2
  exit 2
fi
if [[ ! -f "${PLUGIN_PATH}" ]]; then
  echo "plugin not found: ${PLUGIN_PATH}" >&2
  exit 2
fi

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

PORT_DAEMON="$(agentd_smoke_pick_port)"
PORT_STUB="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"
STUB_BASE="http://${HOST}:${PORT_STUB}/v1"
NAME="agentd_workflow_budget_events_smoke"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

python3 -u - <<PY > "${LOG_DIR}/${NAME}.stub.stdout.log" 2> "${LOG_DIR}/${NAME}.stub.stderr.log" &
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

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
                    "name": "ext_echo",
                    "arguments": json.dumps({"text": "hello from stub"}),
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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}" \
  --tools host \
  --no-yolo \
  --tool-plugin "${PLUGIN_PATH}"

agentd_smoke_wait_health "${DAEMON_URL}"

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
req = {
  "allow_inline_api_keys": True,
  "workflow_limits": {
    "max_tool_calls_total": 1
  },
  "tasks": [
    {
      "task_id":"A",
      "request":{
        "prompt":"Call ext_echo then say OK",
        "no_session": True,
        "tools":"host",
        "yolo": False,
        "host_policy":"readonly",
        "base_url":"${STUB_BASE}",
        "api_key":"dummy",
        "model":"stub",
        "max_steps": 4,
        "verbose": True
      }
    },
    {
      "task_id":"B",
      "depends_on":["A"],
      "request":{
        "prompt":"Call ext_echo then say OK",
        "no_session": True,
        "tools":"host",
        "yolo": False,
        "host_policy":"readonly",
        "base_url":"${STUB_BASE}",
        "api_key":"dummy",
        "model":"stub",
        "max_steps": 4,
        "verbose": True
      }
    }
  ]
}
print(json.dumps(req))
PY
)" \
  "${DAEMON_URL}/api/v1/workflow/submit")"

workflow_id="$(python3 - <<PY
import json
obj = json.loads(r'''${submit_resp}''')
print(obj.get("workflow_id",""))
PY
)"
if [[ -z "${workflow_id}" ]]; then
  echo "failed to get workflow_id: ${submit_resp}" >&2
  exit 1
fi

final=""
for _ in $(seq 1 260); do
  final="$(curl -fsS --noproxy "*" --max-time 5 \
    "${DAEMON_URL}/api/v1/workflow?workflow_id=${workflow_id}&include_tasks=1&include_results=1")"
  if python3 - <<PY >/dev/null 2>&1
import json
obj = json.loads(r'''${final}''')
w = obj.get("workflow") or {}
raise SystemExit(0 if w.get("status") in ("done","error","cancelled") else 1)
PY
  then
    break
  fi
  sleep 0.05
done

events_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/workflow/events?workflow_id=${workflow_id}&after_event_id=0&limit=256")"

python3 - <<PY
import json, sys

def as_int(d, key):
  v = d.get(key)
  if isinstance(v, bool) or not isinstance(v, int):
    return None
  return v

obj = json.loads(r'''${final}''')
if not obj.get("ok"):
  print("workflow get failed", obj, file=sys.stderr)
  raise SystemExit(1)
w = obj.get("workflow") or {}
if w.get("status") != "cancelled":
  print("expected workflow status cancelled", w, file=sys.stderr)
  raise SystemExit(1)

ev_obj = json.loads(r'''${events_resp}''')
if not ev_obj.get("ok"):
  print("workflow events failed", ev_obj, file=sys.stderr)
  raise SystemExit(1)
events = ev_obj.get("events") or []
budget_events = [e for e in events if isinstance(e, dict) and e.get("type") == "workflow_budget_exceeded"]
if len(budget_events) != 1:
  print("expected exactly one workflow_budget_exceeded event", events, file=sys.stderr)
  raise SystemExit(1)
event = budget_events[0]
if event.get("schema") != "run_event_payload_workflow_budget_exceeded_v1":
  print("unexpected schema", event, file=sys.stderr)
  raise SystemExit(1)
data = event.get("data") or {}
if data.get("workflow_id") != "${workflow_id}":
  print("unexpected workflow_id in event", data, file=sys.stderr)
  raise SystemExit(1)
if data.get("reason") != "max_tool_calls_total":
  print("unexpected reason", data, file=sys.stderr)
  raise SystemExit(1)
if as_int(data, "max_tool_calls_total") != 1:
  print("expected max_tool_calls_total == 1", data, file=sys.stderr)
  raise SystemExit(1)
if as_int(data, "tool_calls_used") != 1:
  print("expected tool_calls_used == 1", data, file=sys.stderr)
  raise SystemExit(1)
if as_int(data, "tool_calls_remaining") != 0:
  print("expected tool_calls_remaining == 0", data, file=sys.stderr)
  raise SystemExit(1)
if not isinstance(data.get("ts_unix_ms"), int):
  print("expected ts_unix_ms int", data, file=sys.stderr)
  raise SystemExit(1)
PY

echo "ok: ${NAME}"
