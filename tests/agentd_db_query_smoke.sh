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

DB_PATH="${LOG_DIR}/agentd_db_query_smoke.sqlite"
SESSION_ID="agentd_db_query_smoke_$(date +%s)_$RANDOM"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
  rm -f "${DB_PATH}" "${DB_PATH}-wal" "${DB_PATH}-shm" >/dev/null 2>&1 || true
}
trap cleanup EXIT

# OpenAI-compatible stub:
# - first response: tool call to fs_read README.md
# - second response (after tool result): assistant "OK"
python3 -u - <<PY > "${LOG_DIR}/agentd_db_query_smoke.stub.stdout.log" 2> "${LOG_DIR}/agentd_db_query_smoke.stub.stderr.log" &
import json
from http.server import BaseHTTPRequestHandler, HTTPServer

def has_tool_result(messages):
  for m in messages:
    if isinstance(m, dict) and m.get("role") == "tool":
      return True
  return False

def is_loop_fs_read(messages):
  for m in messages:
    if not isinstance(m, dict):
      continue
    if m.get("role") != "user":
      continue
    c = m.get("content")
    if isinstance(c, str) and "loop_fs_read" in c:
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

    if is_loop_fs_read(messages):
      body = {
        "id": "cmpl_stub_loop",
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
                  "id": "call_loop_1",
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
    elif has_tool_result(messages):
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

rm -f "${DB_PATH}" "${DB_PATH}-wal" "${DB_PATH}-shm" >/dev/null 2>&1 || true

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_db_query_smoke" \
  --tools host \
  --no-yolo \
  --db-path "${DB_PATH}"

agentd_smoke_wait_health "${DAEMON_URL}"

resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "Read README.md then say OK",
  "session_id": "${SESSION_ID}",
  "tools": "host",
  "yolo": False,
  "base_url": "${STUB_BASE}",
  "api_key": "dummy",
  "model": "stub",
  "max_steps": 4,
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
txt = (obj.get("assistant_text") or "").strip()
if txt != "OK":
  print("unexpected assistant_text:", txt, file=sys.stderr)
  raise SystemExit(1)
PY

resp2="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "Read README.md then say OK (but hit max_steps)",
  "session_id": "${SESSION_ID}",
  "tools": "host",
  "yolo": False,
  "base_url": "${STUB_BASE}",
  "api_key": "dummy",
  "model": "stub",
  "max_steps": 1,
  "verbose": True
}))
PY
)" \
  "${DAEMON_URL}/api/v1/run")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp2}''')
if obj.get("ok"):
  print("expected failing run but ok=true:", obj, file=sys.stderr)
  raise SystemExit(1)
err = (obj.get("error") or "").lower()
if "max steps" not in err:
  print("expected max steps error; got:", obj.get("error"), file=sys.stderr)
  raise SystemExit(1)
PY

resp3="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "loop_fs_read: repeatedly read README.md until stopped (should hit per-tool limit)",
  "session_id": "${SESSION_ID}",
  "tools": "host",
  "yolo": False,
  "base_url": "${STUB_BASE}",
  "api_key": "dummy",
  "model": "stub",
  "max_repeated_tool_calls": 0,
  "tool_call_limits": [{"tool":"fs_read","max_calls":2}],
  "verbose": True
}))
PY
)" \
  "${DAEMON_URL}/api/v1/run")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp3}''')
if obj.get("ok"):
  print("expected failing run but ok=true:", obj, file=sys.stderr)
  raise SystemExit(1)
err = (obj.get("error") or "").lower()
if "max tool calls" not in err:
  print("expected tool call limit error; got:", obj.get("error"), file=sys.stderr)
  raise SystemExit(1)
PY

workflow_submit="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
tasks = [
  {"task_id":"wf_a","request":{"prompt":"Workflow A","no_session":True,"tools":"host","base_url":"${STUB_BASE}","api_key":"dummy","model":"stub","trace":False}},
  {"task_id":"wf_b","depends_on":["wf_a"],"request":{"prompt":"Workflow B got \${task.wf_a.json:/assistant_text}","no_session":True,"tools":"host","base_url":"${STUB_BASE}","api_key":"dummy","model":"stub","trace":False}},
]
print(json.dumps({
  "session_id": "${SESSION_ID}",
  "allow_sessions": True,
  "allow_inline_api_keys": True,
  "trace_id": "trace_${SESSION_ID}",
  "tasks": tasks
}))
PY
)" \
  "${DAEMON_URL}/api/v1/workflow/submit")"

WORKFLOW_ID="$(python3 - <<PY
import json
obj = json.loads(r'''${workflow_submit}''')
print(obj.get("workflow_id",""))
PY
)"
if [[ -z "${WORKFLOW_ID}" ]]; then
  echo "failed to get workflow_id: ${workflow_submit}" >&2
  exit 1
fi

final_workflow=""
for _ in $(seq 1 120); do
  final_workflow="$(curl -fsS --noproxy "*" --max-time 5 \
    "${DAEMON_URL}/api/v1/workflow?workflow_id=${WORKFLOW_ID}&include_tasks=1")"
  if python3 - <<PY >/dev/null 2>&1
import json
obj = json.loads(r'''${final_workflow}''')
w = obj.get("workflow") or {}
st = w.get("status")
if st in ("done","error","cancelled"):
  raise SystemExit(0)
raise SystemExit(1)
PY
  then
    break
  fi
  sleep 0.05
done

python3 - <<PY
import json, sys
obj = json.loads(r'''${final_workflow}''')
if not obj.get("ok"):
  print("workflow get failed:", obj, file=sys.stderr)
  raise SystemExit(1)
w = obj.get("workflow") or {}
if w.get("status") != "done":
  print("expected workflow status done:", w, file=sys.stderr)
  raise SystemExit(1)
PY

db_runs="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/runs?session_id=$(python3 -c 'import urllib.parse; print(urllib.parse.quote("""'${SESSION_ID}'"""))')&limit=20&offset=0")"

RUN_ID="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${db_runs}''')
if not obj.get("ok"):
  print("db/runs failed:", obj, file=sys.stderr)
  raise SystemExit(1)
runs = obj.get("runs") or []
if not runs:
  print("expected at least 1 run row", file=sys.stderr)
  raise SystemExit(1)
rid = runs[0].get("run_id")
if not isinstance(rid, int) or rid <= 0:
  print("invalid run_id:", rid, file=sys.stderr)
  raise SystemExit(1)
has_last_error = any(isinstance(r, dict) and isinstance(r.get("last_error"), dict) for r in runs)
if not has_last_error:
  print("expected at least one row with parsed last_error", file=sys.stderr)
  raise SystemExit(1)
print(rid)
PY
)"

db_runs_filtered="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/runs?session_id=$(python3 -c 'import urllib.parse; print(urllib.parse.quote("""'${SESSION_ID}'"""))')&limit=50&offset=0&only_errors=1&stop_reason=max_steps_exceeded")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${db_runs_filtered}''')
if not obj.get("ok"):
  print("db/runs filtered failed:", obj, file=sys.stderr)
  raise SystemExit(1)
rows = obj.get("runs") or []
if not rows:
  print("expected at least 1 filtered error row", file=sys.stderr)
  raise SystemExit(1)
PY

db_run="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/run?run_id=${RUN_ID}&include_events=1&include_tools=1&include_artifacts=1&include_ui_actions=1")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${db_run}''')
if not obj.get("ok"):
  print("db/run failed:", obj, file=sys.stderr)
  raise SystemExit(1)
run = obj.get("run") or {}
if int(run.get("run_id") or 0) != int(${RUN_ID}):
  print("unexpected run_id:", run.get("run_id"), file=sys.stderr)
  raise SystemExit(1)
tools = obj.get("tool_records") or []
if len(tools) < 1:
  print("expected tool_records >= 1", file=sys.stderr)
  raise SystemExit(1)
events = obj.get("events") or []
if not isinstance(events, list) or len(events) < 1:
  print("expected events >= 1", file=sys.stderr)
  raise SystemExit(1)
# include_ui_actions=1 should always return a list (possibly empty).
uia = obj.get("ui_actions")
if not isinstance(uia, list):
  print("expected ui_actions to be a list", type(uia), file=sys.stderr)
  raise SystemExit(1)
# If server parsed data_json successfully, it should expose data for at least some rows.
has_parsed = any(isinstance(e, dict) and isinstance(e.get("data"), dict) for e in events)
if not has_parsed:
  print("expected at least one parsed event.data dict", file=sys.stderr)
  raise SystemExit(1)
PY

db_arts="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/artifacts?session_id=$(python3 -c 'import urllib.parse; print(urllib.parse.quote("""'${SESSION_ID}'"""))')&limit=20&offset=0")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${db_arts}''')
if not obj.get("ok"):
  print("db/artifacts failed:", obj, file=sys.stderr)
  raise SystemExit(1)
PY

db_ui_actions="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/ui_actions?session_id=$(python3 -c 'import urllib.parse; print(urllib.parse.quote("""'${SESSION_ID}'"""))')&limit=20&offset=0")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${db_ui_actions}''')
if not obj.get("ok"):
  print("db/ui_actions failed:", obj, file=sys.stderr)
  raise SystemExit(1)
rows = obj.get("ui_actions")
if rows is None:
  print("expected ui_actions field", file=sys.stderr)
  raise SystemExit(1)
if not isinstance(rows, list):
  print("expected ui_actions list; got:", type(rows), file=sys.stderr)
  raise SystemExit(1)
PY

db_sessions="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/sessions?limit=50&offset=0")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${db_sessions}''')
if not obj.get("ok"):
  print("db/sessions failed:", obj, file=sys.stderr)
  raise SystemExit(1)
rows = obj.get("sessions") or []
if not isinstance(rows, list):
  print("expected sessions list; got:", type(rows), file=sys.stderr)
  raise SystemExit(1)
sid = """${SESSION_ID}"""
if not any(isinstance(r, dict) and r.get("session_id") == sid for r in rows):
  print("expected to find session_id in db/sessions:", sid, file=sys.stderr)
  raise SystemExit(1)
PY

ui_evt_resp="$(curl -fsS --noproxy "*" --max-time 10 -X POST \
  -H 'Content-Type: application/json' \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${SESSION_ID}",
  "type": "smoke_client_event",
  "data": {"k":"v"},
  "append_to_session": True,
}))
PY
)" \
  "${DAEMON_URL}/api/v1/session/client_event")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${ui_evt_resp}''')
if not obj.get("ok"):
  print("session/client_event failed:", obj, file=sys.stderr)
  raise SystemExit(1)
PY

db_msgs="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/messages?session_id=$(python3 -c 'import urllib.parse; print(urllib.parse.quote("""'${SESSION_ID}'"""))')&limit=50&offset=0&max_content_bytes=128")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${db_msgs}''')
if not obj.get("ok"):
  print("db/messages failed:", obj, file=sys.stderr)
  raise SystemExit(1)
rows = obj.get("messages")
if rows is None:
  print("expected messages field", file=sys.stderr)
  raise SystemExit(1)
if not isinstance(rows, list):
  print("expected messages list; got:", type(rows), file=sys.stderr)
  raise SystemExit(1)
if len(rows) < 1:
  print("expected messages >= 1", file=sys.stderr)
  raise SystemExit(1)
m0 = rows[0]
if not isinstance(m0, dict):
  print("expected message row object; got:", type(m0), file=sys.stderr)
  raise SystemExit(1)
# Ensure truncation metadata exists (even if not truncated).
if "content_truncated" not in m0 or "content_bytes" not in m0:
  print("expected content_truncated/content_bytes fields", m0, file=sys.stderr)
  raise SystemExit(1)
if "mm_json" not in m0 or "mm_json_truncated" not in m0 or "mm_bytes" not in m0:
  print("expected mm_json/mm_json_truncated/mm_bytes fields", m0, file=sys.stderr)
  raise SystemExit(1)
if not isinstance(m0.get("mm_json"), str):
  print("expected mm_json to be string", m0, file=sys.stderr)
  raise SystemExit(1)
if not any(isinstance(m, dict) and isinstance(m.get("content"), str) and "[client_event]" in m.get("content") for m in rows):
  print("expected at least one [client_event] message", file=sys.stderr)
  raise SystemExit(1)
PY

db_client_events="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/client_events?session_id=$(python3 -c 'import urllib.parse; print(urllib.parse.quote("""'${SESSION_ID}'"""))')&limit=20&offset=0")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${db_client_events}''')
if not obj.get("ok"):
  print("db/client_events failed:", obj, file=sys.stderr)
  raise SystemExit(1)
rows = obj.get("client_events") or []
if not isinstance(rows, list):
  print("expected client_events list; got:", type(rows), file=sys.stderr)
  raise SystemExit(1)
if not any(isinstance(r, dict) and r.get("type") == "smoke_client_event" for r in rows):
  print("expected to find smoke_client_event", rows, file=sys.stderr)
  raise SystemExit(1)
PY

db_workflows="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/workflows?session_id=$(python3 -c 'import urllib.parse; print(urllib.parse.quote("""'${SESSION_ID}'"""))')&limit=20&offset=0&include_result=1")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${db_workflows}''')
if not obj.get("ok"):
  print("db/workflows failed:", obj, file=sys.stderr)
  raise SystemExit(1)
rows = obj.get("workflows") or []
wid = """${WORKFLOW_ID}"""
if not any(isinstance(r, dict) and r.get("workflow_id") == wid for r in rows):
  print("expected workflow_id in db/workflows:", wid, file=sys.stderr)
  raise SystemExit(1)
PY

db_workflow="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/workflow?workflow_id=${WORKFLOW_ID}&include_tasks=1&include_events=1")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${db_workflow}''')
if not obj.get("ok"):
  print("db/workflow failed:", obj, file=sys.stderr)
  raise SystemExit(1)
w = obj.get("workflow") or {}
if w.get("workflow_id") != """${WORKFLOW_ID}""":
  print("unexpected workflow_id:", w.get("workflow_id"), file=sys.stderr)
  raise SystemExit(1)
tasks = obj.get("tasks") or []
if not tasks:
  print("expected workflow tasks", file=sys.stderr)
  raise SystemExit(1)
events = obj.get("events") or []
if not events:
  print("expected workflow events", file=sys.stderr)
  raise SystemExit(1)
has_parsed = any(isinstance(e, dict) and isinstance(e.get("data"), (dict, list)) for e in events)
if not has_parsed:
  print("expected at least one parsed workflow event data", file=sys.stderr)
  raise SystemExit(1)
PY

db_workflow_tasks="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/workflow_tasks?workflow_id=${WORKFLOW_ID}&limit=20&offset=0")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${db_workflow_tasks}''')
if not obj.get("ok"):
  print("db/workflow_tasks failed:", obj, file=sys.stderr)
  raise SystemExit(1)
rows = obj.get("tasks") or []
if not rows:
  print("expected workflow_tasks rows", file=sys.stderr)
  raise SystemExit(1)
PY

db_workflow_events="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/workflow_events?workflow_id=${WORKFLOW_ID}&limit=50&offset=0")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${db_workflow_events}''')
if not obj.get("ok"):
  print("db/workflow_events failed:", obj, file=sys.stderr)
  raise SystemExit(1)
rows = obj.get("workflow_events") or []
if not rows:
  print("expected workflow_events rows", file=sys.stderr)
  raise SystemExit(1)
missing_schema = [r for r in rows if not isinstance(r, dict) or not isinstance(r.get("schema"), str) or not r.get("schema")]
if missing_schema:
  print("expected workflow_events schema values", file=sys.stderr)
  raise SystemExit(1)
PY

edge_submit="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "workflow_id": "edge_${SESSION_ID}",
  "goal": "edge db query smoke",
  "priority": 1,
  "steps": [
    {"step_id": "s1", "kind": "join", "depends_on": [], "payload": {}}
  ]
}))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/workflow/submit")"

EDGE_WORKFLOW_ID="$(python3 - <<PY
import json
obj = json.loads(r'''${edge_submit}''')
print(obj.get("workflow_id",""))
PY
)"
if [[ -z "${EDGE_WORKFLOW_ID}" ]]; then
  echo "failed to get edge workflow_id: ${edge_submit}" >&2
  exit 1
fi

db_edge_workflows="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/edge_workflows?limit=50&offset=0&include_spec=1")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${db_edge_workflows}''')
if not obj.get("ok"):
  print("db/edge_workflows failed:", obj, file=sys.stderr)
  raise SystemExit(1)
rows = obj.get("edge_workflows") or []
wid = """${EDGE_WORKFLOW_ID}"""
if not any(isinstance(r, dict) and r.get("workflow_id") == wid for r in rows):
  print("expected edge workflow_id in db/edge_workflows:", wid, file=sys.stderr)
  raise SystemExit(1)
PY

db_edge_workflow="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/edge_workflow?workflow_id=${EDGE_WORKFLOW_ID}&include_steps=1&include_events=1")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${db_edge_workflow}''')
if not obj.get("ok"):
  print("db/edge_workflow failed:", obj, file=sys.stderr)
  raise SystemExit(1)
wf = obj.get("edge_workflow") or {}
if wf.get("workflow_id") != """${EDGE_WORKFLOW_ID}""":
  print("unexpected edge workflow_id:", wf.get("workflow_id"), file=sys.stderr)
  raise SystemExit(1)
steps = obj.get("steps") or []
if not steps:
  print("expected edge workflow steps", file=sys.stderr)
  raise SystemExit(1)
PY

db_edge_steps="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/edge_workflow_steps?workflow_id=${EDGE_WORKFLOW_ID}&limit=20&offset=0")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${db_edge_steps}''')
if not obj.get("ok"):
  print("db/edge_workflow_steps failed:", obj, file=sys.stderr)
  raise SystemExit(1)
rows = obj.get("edge_workflow_steps") or []
if not rows:
  print("expected edge workflow_steps rows", file=sys.stderr)
  raise SystemExit(1)
PY

db_edge_events="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/edge_workflow_events?workflow_id=${EDGE_WORKFLOW_ID}&limit=20&offset=0")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${db_edge_events}''')
if not obj.get("ok"):
  print("db/edge_workflow_events failed:", obj, file=sys.stderr)
  raise SystemExit(1)
rows = obj.get("edge_workflow_events") or []
if rows is None:
  print("expected edge_workflow_events field", file=sys.stderr)
  raise SystemExit(1)
missing_schema = [r for r in rows if not isinstance(r, dict) or not isinstance(r.get("schema"), str) or not r.get("schema")]
if missing_schema:
  print("expected edge_workflow_events schema values", file=sys.stderr)
  raise SystemExit(1)
PY

blob_payload="$(python3 - <<'PY'
import base64, json
data = base64.b64encode(b"hello blob").decode("utf-8")
print(json.dumps({"data_base64": data, "mime": "text/plain"}))
PY
)"

blob_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${blob_payload}" \
  "${DAEMON_URL}/api/v1/blob/upload")"

BLOB_ID="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${blob_resp}''')
if not obj.get("ok"):
  print("blob/upload failed:", obj, file=sys.stderr)
  raise SystemExit(1)
print(obj.get("blob_id") or "")
PY
)"

if [[ -z "${BLOB_ID}" ]]; then
  echo "blob/upload missing blob_id" >&2
  exit 1
fi

BLOB_ID_ENC="$(BLOB_ID="${BLOB_ID}" python3 - <<'PY'
import os
from urllib.parse import quote
print(quote(os.environ["BLOB_ID"]))
PY
)"

db_blobs="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/blobs?limit=20&offset=0")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${db_blobs}''')
if not obj.get("ok"):
  print("db/blobs failed:", obj, file=sys.stderr)
  raise SystemExit(1)
rows = obj.get("blobs") or []
blob_ids = {r.get("blob_id") for r in rows if isinstance(r, dict)}
if "${BLOB_ID}" not in blob_ids:
  print("expected blob_id in db/blobs", "${BLOB_ID}", blob_ids, file=sys.stderr)
  raise SystemExit(1)
PY

db_blob="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/blob?blob_id=${BLOB_ID_ENC}&include_artifacts=1")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${db_blob}''')
if not obj.get("ok"):
  print("db/blob failed:", obj, file=sys.stderr)
  raise SystemExit(1)
blob = obj.get("blob") or {}
if blob.get("blob_id") != "${BLOB_ID}":
  print("db/blob missing blob_id", blob, file=sys.stderr)
  raise SystemExit(1)
if "artifacts" not in obj:
  print("db/blob missing artifacts field", obj, file=sys.stderr)
  raise SystemExit(1)
PY

db_blob_analytics="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/analytics/blobs?top_mime_limit=5")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${db_blob_analytics}''')
if not obj.get("ok"):
  print("db/analytics/blobs failed:", obj, file=sys.stderr)
  raise SystemExit(1)
totals = obj.get("totals") or {}
if (totals.get("blob_count") or 0) < 1:
  print("expected blob_count >= 1", obj, file=sys.stderr)
  raise SystemExit(1)
tiers = obj.get("by_tier") or []
if not tiers:
  print("expected by_tier entries", obj, file=sys.stderr)
  raise SystemExit(1)
PY

db_analytics="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/analytics/workflows?scope=all")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${db_analytics}''')
if not obj.get("ok"):
  print("db/analytics/workflows failed:", obj, file=sys.stderr)
  raise SystemExit(1)
if "durable" not in obj or "edge" not in obj:
  print("expected durable+edge analytics sections", obj, file=sys.stderr)
  raise SystemExit(1)
PY

db_workflow_export_json="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/analytics/workflows/export?format=json&scope=all")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${db_workflow_export_json}''')
if not obj.get("ok"):
  print("db/analytics/workflows/export json failed:", obj, file=sys.stderr)
  raise SystemExit(1)
if "generated_utc_ms" not in obj:
  print("expected generated_utc_ms in workflow export", obj, file=sys.stderr)
  raise SystemExit(1)
if "durable" not in obj or "edge" not in obj:
  print("expected durable+edge in workflow export", obj, file=sys.stderr)
  raise SystemExit(1)
PY

db_workflow_export_csv="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/analytics/workflows/export?format=csv&scope=durable")"

python3 - <<PY
import sys
data = r'''${db_workflow_export_csv}'''
if "section,metric,key,value" not in data:
  print("expected csv header in workflow export", file=sys.stderr)
  raise SystemExit(1)
if "durable" not in data:
  print("expected durable rows in workflow export csv", file=sys.stderr)
  raise SystemExit(1)
PY

db_edge_analytics="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/analytics/edge?active_within_ms=600000")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${db_edge_analytics}''')
if not obj.get("ok"):
  print("db/analytics/edge failed:", obj, file=sys.stderr)
  raise SystemExit(1)
if "edge_tasks" not in obj or "edge_nodes" not in obj:
  print("expected edge_tasks + edge_nodes sections", obj, file=sys.stderr)
  raise SystemExit(1)
PY

db_edge_export_json="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/analytics/edge/export?format=json&scope=all&active_within_ms=600000")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${db_edge_export_json}''')
if not obj.get("ok"):
  print("db/analytics/edge/export json failed:", obj, file=sys.stderr)
  raise SystemExit(1)
if "generated_utc_ms" not in obj:
  print("expected generated_utc_ms", obj, file=sys.stderr)
  raise SystemExit(1)
if "edge_tasks" not in obj or "edge_nodes" not in obj:
  print("expected edge_tasks + edge_nodes in export json", obj, file=sys.stderr)
  raise SystemExit(1)
PY

db_edge_export_csv="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/analytics/edge/export?format=csv&scope=edge_tasks&active_within_ms=600000")"

python3 - <<PY
import sys
data = r'''${db_edge_export_csv}'''
if "section,metric,key,value" not in data:
  print("expected csv header in export", file=sys.stderr)
  raise SystemExit(1)
if "edge_tasks" not in data:
  print("expected edge_tasks rows in export csv", file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_db_query_smoke OK"
