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

# Tiny OpenAI-compatible stub:
# - first response: returns a tool call
# - second response (after tool result): returns assistant content "OK"
python3 -u - <<PY > "${LOG_DIR}/agentd_trace_id_smoke.stub.stdout.log" 2> "${LOG_DIR}/agentd_trace_id_smoke.stub.stderr.log" &
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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_trace_id_smoke" \
  --tools host \
  --no-yolo

agentd_smoke_wait_health "${DAEMON_URL}"

# Sync run: client-provided trace_id round-trips and is present on every event.
TRACE_SYNC="trace_sync_$(date +%s)_$RANDOM"
resp_sync="$(curl -fsS --noproxy "*" --max-time 15 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "Read README.md then say OK",
  "no_session": True,
  "tools": "host",
  "yolo": False,
  "base_url": "${STUB_BASE}",
  "api_key": "dummy",
  "model": "stub",
  "max_steps": 4,
  "trace_id": "${TRACE_SYNC}"
}))
PY
)" \
  "${DAEMON_URL}/api/v1/run")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp_sync}''')
if not obj.get("ok"):
  print("sync run failed:", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("trace_id") != "${TRACE_SYNC}":
  print("missing/incorrect trace_id in response:", obj.get("trace_id"), file=sys.stderr)
  raise SystemExit(1)
events = obj.get("events")
if not isinstance(events, list) or not events:
  print("missing events array", file=sys.stderr)
  raise SystemExit(1)
for i, e in enumerate(events):
  if not isinstance(e, dict):
    continue
  if e.get("trace_id") != "${TRACE_SYNC}":
    print(f"event[{i}] missing/incorrect trace_id:", e.get("trace_id"), file=sys.stderr)
    raise SystemExit(1)
types = [e.get("type") for e in events if isinstance(e, dict)]
if "tool_call" not in types or "tool_result" not in types:
  print("expected tool_call/tool_result events; got:", types, file=sys.stderr)
  raise SystemExit(1)
txt = (obj.get("assistant_text") or "").strip()
if txt != "OK":
  print("unexpected assistant_text:", txt, file=sys.stderr)
  raise SystemExit(1)
PY

# Async run: daemon-generated trace_id is stable across run_async response, SSE agent_event, and job_done.
job_json="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "Read README.md then say OK",
  "no_session": True,
  "tools": "host",
  "yolo": False,
  "base_url": "${STUB_BASE}",
  "api_key": "dummy",
  "model": "stub",
  "max_steps": 4
}))
PY
)" \
  "${DAEMON_URL}/api/v1/run_async")"

job_id="$(python3 - <<PY
import json, sys
obj=json.loads(r'''${job_json}''')
if not obj.get("ok") or not obj.get("job_id"):
  print("bad run_async response", obj, file=sys.stderr)
  raise SystemExit(1)
if not obj.get("trace_id"):
  print("missing trace_id in run_async response", obj, file=sys.stderr)
  raise SystemExit(1)
print(obj["job_id"])
PY
)"

TRACE_ASYNC="$(python3 - <<PY
import json
obj=json.loads(r'''${job_json}''')
print(obj.get("trace_id") or "")
PY
)"

OUT_FILE="${LOG_DIR}/agentd_trace_id_smoke.stream.log"
rm -f "${OUT_FILE}"
curl -fsS --noproxy "*" --max-time 20 -N \
  "${DAEMON_URL}/api/v1/job/stream?job_id=$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote('${job_id}'))
PY
)&cursor=0" > "${OUT_FILE}"

python3 - <<PY
import json, sys
trace_expected = "${TRACE_ASYNC}"
if not trace_expected:
  print("missing TRACE_ASYNC", file=sys.stderr)
  raise SystemExit(1)

cur_event = None
seen_agent = 0
seen_done = 0
with open("${OUT_FILE}", "r", encoding="utf-8") as f:
  for raw in f:
    line = raw.rstrip("\n")
    if line.startswith("event: "):
      cur_event = line[len("event: "):].strip()
    if not line.startswith("data: "):
      continue
    data = line[len("data: "):].strip()
    try:
      obj = json.loads(data)
    except Exception:
      continue
    if cur_event == "agent_event":
      seen_agent += 1
      got = obj.get("trace_id")
      if got != trace_expected:
        print("agent_event trace_id mismatch:", got, "expected:", trace_expected, file=sys.stderr)
        raise SystemExit(1)
    if cur_event == "job_done":
      seen_done += 1
      got = obj.get("trace_id") or (obj.get("result") or {}).get("trace_id")
      if got != trace_expected:
        print("job_done trace_id mismatch:", got, "expected:", trace_expected, file=sys.stderr)
        raise SystemExit(1)

if seen_agent == 0:
  print("expected at least one agent_event in SSE stream", file=sys.stderr)
  raise SystemExit(1)
if seen_done == 0:
  print("expected job_done in SSE stream", file=sys.stderr)
  raise SystemExit(1)
PY

# Best-effort cleanup.
curl -fsS --noproxy "*" -X DELETE "${DAEMON_URL}/api/v1/job?job_id=$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote('${job_id}'))
PY
)" --max-time 5 >/dev/null || true

echo "agentd_trace_id_smoke OK"

