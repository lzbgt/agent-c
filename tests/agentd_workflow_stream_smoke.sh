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

DB_PATH="${LOG_DIR}/agentd_workflow_stream_smoke_${PORT_DAEMON}.sqlite"
STATE_DIR="${LOG_DIR}/agentd_workflow_stream_smoke_${PORT_DAEMON}.state"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

# OpenAI-compatible stub: echoes user prompt as assistant content.
python3 -u - <<PY > "${LOG_DIR}/agentd_workflow_stream_smoke.stub.stdout.log" 2> "${LOG_DIR}/agentd_workflow_stream_smoke.stub.stderr.log" &
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

def last_user_prompt(req):
  msgs = req.get("messages") or []
  for m in reversed(msgs):
    if isinstance(m, dict) and m.get("role") == "user":
      c = m.get("content")
      if isinstance(c, str):
        return c
  return ""

class H(BaseHTTPRequestHandler):
  def log_message(self, fmt, *args):
    return
  def do_POST(self):
    if self.path != "/v1/chat/completions":
      self.send_response(404)
      self.end_headers()
      return
    raw = self.rfile.read(int(self.headers.get("Content-Length","0") or "0"))
    try:
      req = json.loads(raw.decode("utf-8"))
    except Exception:
      self.send_response(400)
      self.end_headers()
      return
    prompt = last_user_prompt(req)
    body = {
      "id": "cmpl_stub",
      "object": "chat.completion",
      "created": 0,
      "model": "stub",
      "choices": [
        {"index": 0, "message": {"role": "assistant", "content": prompt}, "finish_reason": "stop"}
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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_workflow_stream_smoke" \
  --db-path "${DB_PATH}" \
  --state-dir "${STATE_DIR}" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
tasks = [
  {"task_id":"A","request":{"prompt":"Alpha","no_session":True,"tools":"none","base_url":"${STUB_BASE}","api_key":"dummy","model":"stub","trace":False}},
  {"task_id":"B","depends_on":["A"],"request":{"prompt":"B got \${task.A.json:/assistant_text}","no_session":True,"tools":"none","base_url":"${STUB_BASE}","api_key":"dummy","model":"stub","trace":False}}
]
print(json.dumps({"tasks": tasks, "allow_inline_api_keys": True}))
PY
)" \
  "${DAEMON_URL}/api/v1/workflow/submit")"

workflow_id="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${submit_resp}''')
wid = obj.get("workflow_id","")
if not obj.get("ok") or not wid:
  print("bad submit response", obj, file=sys.stderr)
  raise SystemExit(1)
print(wid)
PY
)"

OUT_FILE="${LOG_DIR}/agentd_workflow_stream_smoke.stream.log"
rm -f "${OUT_FILE}"

curl -fsS --noproxy "*" --max-time 20 -N \
  "${DAEMON_URL}/api/v1/workflow/stream?workflow_id=$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote('${workflow_id}'))
PY
)&cursor=0" > "${OUT_FILE}"

if ! grep -q "event: workflow_event" "${OUT_FILE}"; then
  echo "expected SSE to contain workflow_event" >&2
  tail -n 80 "${OUT_FILE}" >&2 || true
  exit 1
fi
if ! grep -q "event: workflow_done" "${OUT_FILE}"; then
  echo "expected SSE to contain workflow_done" >&2
  tail -n 80 "${OUT_FILE}" >&2 || true
  exit 1
fi
if ! grep -q "\"type\":\"task_status\"" "${OUT_FILE}"; then
  echo "expected SSE payload to mention task_status" >&2
  tail -n 80 "${OUT_FILE}" >&2 || true
  exit 1
fi

workflow_id_q="$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote('''${workflow_id}'''))
PY
)"
events_all="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/workflow/events?workflow_id=${workflow_id_q}&after_event_id=0&limit=256")"
events_task_b="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/workflow/events?workflow_id=${workflow_id_q}&after_event_id=0&limit=256&task_id=B")"
events_task_status="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/workflow/events?workflow_id=${workflow_id_q}&after_event_id=0&limit=256&event_type=task_status")"
events_task_a_status="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/workflow/events?workflow_id=${workflow_id_q}&after_event_id=0&limit=256&task_id=A&event_type=task_status")"

python3 - <<PY
import json, sys
all_obj = json.loads(r'''${events_all}''')
task_b_obj = json.loads(r'''${events_task_b}''')
task_status_obj = json.loads(r'''${events_task_status}''')
task_a_status_obj = json.loads(r'''${events_task_a_status}''')

for label, obj in [
  ("all", all_obj),
  ("task_b", task_b_obj),
  ("task_status", task_status_obj),
  ("task_a_status", task_a_status_obj),
]:
  if not obj.get("ok"):
    print(f"{label} events query failed: {obj}", file=sys.stderr)
    raise SystemExit(1)

all_events = all_obj.get("events") or []
if not all_events:
  print("expected unfiltered workflow events", file=sys.stderr)
  raise SystemExit(1)
if not any(e.get("task_id") == "A" for e in all_events if isinstance(e, dict)):
  print("expected task A events in unfiltered response", all_events, file=sys.stderr)
  raise SystemExit(1)
if not any(e.get("task_id") == "B" for e in all_events if isinstance(e, dict)):
  print("expected task B events in unfiltered response", all_events, file=sys.stderr)
  raise SystemExit(1)

task_b_events = task_b_obj.get("events") or []
if not task_b_events or any(e.get("task_id") != "B" for e in task_b_events if isinstance(e, dict)):
  print("task_id filter leaked non-B events", task_b_obj, file=sys.stderr)
  raise SystemExit(1)
if task_b_obj.get("task_id") != "B":
  print("task_id filter was not echoed", task_b_obj, file=sys.stderr)
  raise SystemExit(1)

task_status_events = task_status_obj.get("events") or []
if not task_status_events or any(e.get("type") != "task_status" for e in task_status_events if isinstance(e, dict)):
  print("event_type filter leaked non-task_status events", task_status_obj, file=sys.stderr)
  raise SystemExit(1)
if task_status_obj.get("event_type") != "task_status":
  print("event_type filter was not echoed", task_status_obj, file=sys.stderr)
  raise SystemExit(1)

task_a_status_events = task_a_status_obj.get("events") or []
if not task_a_status_events:
  print("expected combined task_id/event_type events", task_a_status_obj, file=sys.stderr)
  raise SystemExit(1)
if any(e.get("task_id") != "A" or e.get("type") != "task_status" for e in task_a_status_events if isinstance(e, dict)):
  print("combined filters leaked mismatched events", task_a_status_obj, file=sys.stderr)
  raise SystemExit(1)
PY
