#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
PLUGIN_PATH="${2:-}"
if [[ -z "${AGENTD_BIN}" || -z "${PLUGIN_PATH}" ]]; then
  echo "usage: agentd_workflow_expect_tool_calls_smoke.sh <agentd_bin> <plugin_path>" >&2
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
NAME="agentd_workflow_expect_tool_calls_smoke"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

# OpenAI-compatible stub: forces a single ext_echo tool call, then returns OK after tool result is present.
python3 -u - <<PY > "${LOG_DIR}/${NAME}.stub.stdout.log" 2> "${LOG_DIR}/${NAME}.stub.stderr.log" &
import json, threading
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
      },
      "expect":{
        "tool_called":"ext_echo",
        "tool_calls_total_between":{"min":1,"max":1},
        "tool_calls_for_tool_between":{"tool":"ext_echo","min":1,"max":1}
      }
    },
    {
      "task_id":"B",
      "allow_error": True,
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
      },
      "expect":{
        "tool_not_called":"ext_echo"
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
import json, sys
obj = json.loads(r'''${final}''')
w = obj.get("workflow") or {}
raise SystemExit(0 if w.get("status") in ("done","error","cancelled") else 1)
PY
  then
    break
  fi
  sleep 0.05
done

python3 - <<PY
import json, sys
obj = json.loads(r'''${final}''')
if not obj.get("ok"):
  print("workflow get failed", obj, file=sys.stderr)
  raise SystemExit(1)
w = obj.get("workflow") or {}
if w.get("status") != "done":
  print("expected workflow status done (B is allow_error)", w, file=sys.stderr)
  raise SystemExit(1)

tasks = obj.get("tasks") or []
by_id = {t.get("task_id"): t for t in tasks if isinstance(t, dict)}
if by_id.get("A", {}).get("status") != "done":
  print("expected A done", by_id.get("A"), file=sys.stderr)
  raise SystemExit(1)
if by_id.get("B", {}).get("status") != "error":
  print("expected B error (failed expectation)", by_id.get("B"), file=sys.stderr)
  raise SystemExit(1)

err = (by_id.get("B", {}) or {}).get("error") or ""
if "forbidden tool was called" not in err:
  print("expected tool-not-called expectation error", err, file=sys.stderr)
  raise SystemExit(1)
PY

