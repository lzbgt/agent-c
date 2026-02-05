#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
PLUGIN_PATH="${2:-}"
if [[ -z "${AGENTD_BIN}" || -z "${PLUGIN_PATH}" ]]; then
  echo "usage: agentd_workflow_budget_tokens_stream_smoke.sh <agentd_bin> <plugin_path>" >&2
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
NAME="agentd_workflow_budget_tokens_stream_smoke"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

# OpenAI-compatible streaming stub with usage in stream chunks:
# - Requires request.stream=true AND stream_options.include_usage=true
# - Step 1: forces a single ext_echo tool call, then ends stream with usage.total_tokens=10
# - Step 2: returns "OK" and ends stream with usage.total_tokens=10
# => One task consumes exactly 20 total tokens in provider-reported usage, even in streaming mode.
python3 -u - <<PY > "${LOG_DIR}/${NAME}.stub.stdout.log" 2> "${LOG_DIR}/${NAME}.stub.stderr.log" &
import json, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

def has_tool_result(messages):
  for m in messages:
    if isinstance(m, dict) and m.get("role") == "tool":
      return True
  return False

def usage_obj():
  return {"prompt_tokens": 6, "completion_tokens": 4, "total_tokens": 10}

def sse_send(handler, obj):
  data = json.dumps(obj, separators=(",", ":")).encode("utf-8")
  handler.wfile.write(b"data: " + data + b"\n\n")
  handler.wfile.flush()

def sse_done(handler):
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

    if not req.get("stream"):
      self.send_response(400)
      data = json.dumps({"error":{"message":"stream=true required"}}).encode("utf-8")
      self.send_header("Content-Type", "application/json; charset=utf-8")
      self.send_header("Content-Length", str(len(data)))
      self.end_headers()
      self.wfile.write(data)
      return

    so = req.get("stream_options") if isinstance(req.get("stream_options"), dict) else {}
    if not so.get("include_usage"):
      self.send_response(400)
      data = json.dumps({"error":{"message":"stream_options.include_usage=true required"}}).encode("utf-8")
      self.send_header("Content-Type", "application/json; charset=utf-8")
      self.send_header("Content-Length", str(len(data)))
      self.end_headers()
      self.wfile.write(data)
      return

    messages = req.get("messages") if isinstance(req.get("messages"), list) else []

    self.send_response(200)
    self.send_header("Content-Type", "text/event-stream; charset=utf-8")
    self.send_header("Cache-Control", "no-cache")
    self.end_headers()

    if has_tool_result(messages):
      # Content delta then finish+usage.
      sse_send(self, {"id":"cmpl_stub_2","choices":[{"index":0,"delta":{"content":"OK"},"finish_reason":None}]})
      sse_send(self, {"id":"cmpl_stub_2","choices":[{"index":0,"delta":{},"finish_reason":"stop"}],"usage":usage_obj()})
      sse_done(self)
      return

    # Tool call delta + finish+usage.
    sse_send(self, {"id":"cmpl_stub_1","choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"call_1","function":{"name":"ext_echo","arguments":"{\\"text\\":\\"hello from stub\\"}"}}]},"finish_reason":None}]})
    sse_send(self, {"id":"cmpl_stub_1","choices":[{"index":0,"delta":{},"finish_reason":"tool_calls"}],"usage":usage_obj()})
    sse_done(self)

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
    "max_total_tokens": 20
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
        "max_steps": 10,
        "stream_assistant": True,
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
        "max_steps": 10,
        "stream_assistant": True,
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

python3 - <<PY
import json, sys
obj = json.loads(r'''${final}''')
if not obj.get("ok"):
  print("workflow get failed", obj, file=sys.stderr)
  raise SystemExit(1)
w = obj.get("workflow") or {}
if w.get("status") != "cancelled":
  print("expected workflow status cancelled (token budget exceeded)", w, file=sys.stderr)
  raise SystemExit(1)
if not str(w.get("error","")).startswith("workflow budget exceeded"):
  print("expected workflow error to mention budget exceeded", w, file=sys.stderr)
  raise SystemExit(1)

usage = obj.get("workflow_usage") or {}
if int((usage.get("total_tokens_used") or -1)) != 20:
  print("expected workflow_usage.total_tokens_used==20", usage, file=sys.stderr)
  raise SystemExit(1)

tasks = obj.get("tasks") or []
by_id = {t.get("task_id"): t for t in tasks if isinstance(t, dict)}
if by_id.get("A", {}).get("status") != "done":
  print("expected A done", by_id.get("A"), file=sys.stderr)
  raise SystemExit(1)
if by_id.get("B", {}).get("status") != "cancelled":
  print("expected B cancelled by budget", by_id.get("B"), file=sys.stderr)
  raise SystemExit(1)

resA = by_id.get("A", {}).get("result") or {}
total_tokens = resA.get("total_tokens")
if total_tokens is None or int(total_tokens) != 20:
  print("expected A total_tokens==20", total_tokens, file=sys.stderr)
  raise SystemExit(1)
PY

echo "ok: streaming token usage counted and token budget enforced"

