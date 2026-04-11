#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
PLUGIN_PATH="${2:-}"
if [[ -z "${AGENTD_BIN}" || -z "${PLUGIN_PATH}" ]]; then
  echo "usage: agentd_workflow_budget_tokens_stream_fallback_smoke.sh <agentd_bin> <plugin_path>" >&2
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
NAME="agentd_workflow_budget_tokens_stream_fallback_smoke"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

# OpenAI-compatible streaming stub:
# - Rejects stream_options.include_usage to force the compatibility retry path.
# - Succeeds only after the client retries without stream_options and then survives one transient 429.
# - Requires max_completion_tokens to equal the remaining workflow token budget on fallback/retry attempts.
# - Streams no usage object, so agentd must charge a fallback budget estimate.
python3 -u - <<PY > "${LOG_DIR}/${NAME}.stub.stdout.log" 2> "${LOG_DIR}/${NAME}.stub.stderr.log" &
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

STATE = {"stream_options_rejections": 0, "fallback_calls": 0}

def has_tool_result(messages):
  for m in messages:
    if isinstance(m, dict) and m.get("role") == "tool":
      return True
  return False

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

    if "stream_options" in req:
      STATE["stream_options_rejections"] += 1
      self.send_response(400)
      data = json.dumps({"error":{"message":"unknown field: stream_options.include_usage"}}).encode("utf-8")
      self.send_header("Content-Type", "application/json; charset=utf-8")
      self.send_header("Content-Length", str(len(data)))
      self.end_headers()
      self.wfile.write(data)
      return

    if int(req.get("max_completion_tokens") or 0) != 1:
      self.send_response(400)
      data = json.dumps({"error":{"message":"max_completion_tokens=1 required on fallback request"}}).encode("utf-8")
      self.send_header("Content-Type", "application/json; charset=utf-8")
      self.send_header("Content-Length", str(len(data)))
      self.end_headers()
      self.wfile.write(data)
      return

    STATE["fallback_calls"] += 1
    if STATE["fallback_calls"] == 1:
      self.send_response(429)
      data = json.dumps({"error":{"message":"simulated transient fallback retry"}}).encode("utf-8")
      self.send_header("Content-Type", "application/json; charset=utf-8")
      self.send_header("Retry-After", "1")
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
      sse_send(self, {"id":"cmpl_stub_2","choices":[{"index":0,"delta":{"content":"OK"},"finish_reason":None}]})
      sse_send(self, {"id":"cmpl_stub_2","choices":[{"index":0,"delta":{},"finish_reason":"stop"}]})
      sse_done(self)
      return

    sse_send(self, {"id":"cmpl_stub_1","choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"call_1","function":{"name":"ext_echo","arguments":"{\\"text\\":\\"hello from fallback stub\\"}"}}]},"finish_reason":None}]})
    sse_send(self, {"id":"cmpl_stub_1","choices":[{"index":0,"delta":{},"finish_reason":"tool_calls"}]})
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
    "max_total_tokens": 1
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
        "max_retries": 2,
        "retry_base_ms": 1,
        "retry_max_ms": 5,
        "retry_jitter": 0.0,
        "respect_retry_after": False,
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
        "max_retries": 2,
        "retry_base_ms": 1,
        "retry_max_ms": 5,
        "retry_jitter": 0.0,
        "respect_retry_after": False,
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
  print("expected workflow status cancelled (fallback token budget exceeded)", w, file=sys.stderr)
  raise SystemExit(1)
if "workflow budget exceeded" not in str(w.get("error","")):
  print("expected workflow error to mention budget exceeded", w, file=sys.stderr)
  raise SystemExit(1)

usage = obj.get("workflow_usage") or {}
if int((usage.get("total_tokens_used") or 0)) <= 0:
  print("expected fallback workflow_usage.total_tokens_used > 0", usage, file=sys.stderr)
  raise SystemExit(1)

tasks = obj.get("tasks") or []
by_id = {t.get("task_id"): t for t in tasks if isinstance(t, dict)}
if by_id.get("A", {}).get("status") != "done":
  print("expected A done", by_id.get("A"), file=sys.stderr)
  raise SystemExit(1)
if by_id.get("B", {}).get("status") != "cancelled":
  print("expected B cancelled by fallback token budget", by_id.get("B"), file=sys.stderr)
  raise SystemExit(1)

resA = by_id.get("A", {}).get("result") or {}
total_tokens = int(resA.get("total_tokens") or 0)
if total_tokens <= 0:
  print("expected A total_tokens fallback charge > 0", resA, file=sys.stderr)
  raise SystemExit(1)

events = resA.get("events") if isinstance(resA.get("events"), list) else []
fallback = []
for ev in events:
  if not isinstance(ev, dict) or ev.get("type") != "llm_usage":
    continue
  data = ev.get("data") if isinstance(ev.get("data"), dict) else {}
  if data.get("estimated") is True and data.get("source") == "stream_fallback_missing_provider_usage":
    fallback.append(data)
if not fallback:
  print("expected estimated stream fallback llm_usage event", events, file=sys.stderr)
  raise SystemExit(1)
retry_events = []
for ev in events:
  if not isinstance(ev, dict) or ev.get("type") != "retry":
    continue
  data = ev.get("data") if isinstance(ev.get("data"), dict) else {}
  retry_events.append(data)
if not any(data.get("reason") == "stream_options_rejected" for data in retry_events):
  print("expected stream_options compatibility retry event", retry_events, file=sys.stderr)
  raise SystemExit(1)
if not any(str(data.get("reason","")).startswith("http_429") for data in retry_events):
  print("expected provider 429 retry event", retry_events, file=sys.stderr)
  raise SystemExit(1)
PY

echo "ok: ${NAME}"
