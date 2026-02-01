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

# Start a tiny OpenAI-compatible stub that:
# - first response: returns a tool call to shell_exec (sleep 2; echo OK)
# - second response (after tool result): returns assistant content "OK"
python3 -u - <<PY > "${LOG_DIR}/agentd_local_async_host_tools_smoke.stub.stdout.log" 2> "${LOG_DIR}/agentd_local_async_host_tools_smoke.stub.stderr.log" &
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
                    "name": "shell_exec",
                    "arguments": json.dumps({"cmd": "sleep 2; echo OK", "timeout_ms": 8000, "max_output_bytes": 65536}),
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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_local_async_host_tools_smoke" \
  --tools host

agentd_smoke_wait_health "${DAEMON_URL}"

job_json="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "Run sleep 2 then say OK",
  "no_session": True,
  "tools": "host",
  "yolo": True,
  "base_url": "${STUB_BASE}",
  "api_key": "dummy",
  "model": "stub",
  "max_steps": 4,
  "verbose": True,
  "trace": False
}))
PY
)" \
  "${DAEMON_URL}/api/v1/run_async")"

job_id="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${job_json}''')
if not obj.get("ok") or not obj.get("job_id"):
  print("bad run_async response", obj, file=sys.stderr)
  raise SystemExit(1)
print(obj["job_id"])
PY
)"

deadline=$(( $(date +%s) + 60 ))
cursor=0
saw_tool_call=0
saw_tool_result=0
saw_heartbeat_after_tool_call=0

while true; do
  now=$(date +%s)
  if [[ "${now}" -gt "${deadline}" ]]; then
    echo "timed out waiting for job ${job_id}" >&2
    exit 1
  fi

  st="$(curl -fsS --noproxy "*" --max-time 10 \
    "${DAEMON_URL}/api/v1/job?job_id=$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote('${job_id}'))
PY
)&include_events=1&cursor=${cursor}&max_events=256")"

  scan="$(python3 - <<PY
import json
obj=json.loads(r'''${st}''')
events=obj.get("events") or []
status=obj.get("status") or ""
next=int(obj.get("events_cursor_next") or 0)
tool_call=0
tool_result=0
heartbeat=0
for e in events:
  t=e.get("type")
  if t=="tool_call":
    d=e.get("data") or {}
    if d.get("tool_name")=="shell_exec":
      tool_call=1
  if t=="tool_result":
    d=e.get("data") or {}
    if d.get("tool_name")=="shell_exec":
      tool_result=1
  if t=="heartbeat":
    heartbeat=1
print(tool_call, tool_result, heartbeat, status, next)
PY
)"

  tc="$(echo "${scan}" | awk '{print $1}')"
  tr="$(echo "${scan}" | awk '{print $2}')"
  hb="$(echo "${scan}" | awk '{print $3}')"
  status="$(echo "${scan}" | awk '{print $4}')"
  next="$(echo "${scan}" | awk '{print $5}')"

  if [[ "${tc}" == "1" ]]; then
    saw_tool_call=1
  fi
  if [[ "${tr}" == "1" ]]; then
    saw_tool_result=1
  fi
  if [[ "${hb}" == "1" && "${saw_tool_call}" == "1" ]]; then
    saw_heartbeat_after_tool_call=1
  fi

  if [[ "${status}" == "done" || "${status}" == "error" ]]; then
    break
  fi
  cursor="${next}"
  sleep 0.2
done

final="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/job?job_id=$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote('${job_id}'))
PY
)")"

python3 - <<PY
import json, sys
obj=json.loads(r'''${final}''')
if not obj.get("ok"):
  print("job query failed", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("status") != "done":
  print("expected done status", obj, file=sys.stderr)
  raise SystemExit(1)
res=obj.get("result") or {}
if not isinstance(res, dict) or not res.get("ok"):
  print("job result not ok", obj, file=sys.stderr)
  raise SystemExit(1)
txt=(res.get("assistant_text") or "").strip()
if txt != "OK":
  print(f"unexpected assistant_text: {txt!r}", file=sys.stderr)
  raise SystemExit(1)
PY

if [[ "${saw_tool_call}" -ne 1 ]]; then
  echo "expected shell_exec tool_call event" >&2
  exit 1
fi
if [[ "${saw_tool_result}" -ne 1 ]]; then
  echo "expected shell_exec tool_result event" >&2
  exit 1
fi
if [[ "${saw_heartbeat_after_tool_call}" -ne 1 ]]; then
  echo "expected heartbeat event after tool_call (daemon heartbeat thread)" >&2
  exit 1
fi

# SSE stream should contain agent events and a job_done marker.
OUT_FILE="${LOG_DIR}/agentd_local_async_host_tools_smoke.stream.log"
rm -f "${OUT_FILE}"
curl -fsS --noproxy "*" --max-time 15 -N \
  "${DAEMON_URL}/api/v1/job/stream?job_id=$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote('${job_id}'))
PY
)&cursor=0" > "${OUT_FILE}"

if ! grep -q "event: agent_event" "${OUT_FILE}"; then
  echo "expected SSE to contain agent_event" >&2
  tail -n 80 "${OUT_FILE}" >&2 || true
  exit 1
fi
if ! grep -q "event: job_done" "${OUT_FILE}"; then
  echo "expected SSE to contain job_done" >&2
  tail -n 80 "${OUT_FILE}" >&2 || true
  exit 1
fi

# Best-effort cleanup.
curl -fsS --noproxy "*" -X DELETE "${DAEMON_URL}/api/v1/job?job_id=$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote('${job_id}'))
PY
)" --max-time 5 >/dev/null || true
