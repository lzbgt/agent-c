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

# OpenAI-compatible stub:
# - returns HTTP 500 when the prompt is exactly "fail"
# - otherwise echoes prompt as assistant content
python3 -u - <<PY > "${LOG_DIR}/agentd_workflow_aggregate_first_ok_smoke.stub.stdout.log" 2> "${LOG_DIR}/agentd_workflow_aggregate_first_ok_smoke.stub.stderr.log" &
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
    prompt = last_user_prompt(req).strip()
    if prompt == "fail":
      self.send_response(500)
      self.end_headers()
      return

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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_workflow_aggregate_first_ok_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
req_fail = {"prompt":"fail","no_session":True,"tools":"none","base_url":"${STUB_BASE}","api_key":"dummy","model":"stub","trace":False}
req_ok = {"prompt":"ok","no_session":True,"tools":"none","base_url":"${STUB_BASE}","api_key":"dummy","model":"stub","trace":False}
tasks = [
  {"task_id":"A","request":req_fail, "max_attempts": 1, "allow_error": True},
  {"task_id":"B","request":req_ok, "max_attempts": 1},
  {"task_id":"J1","kind":"aggregate","depends_on":["A","B"],
   "aggregate":{"mode":"first_ok","task_ids":["A","B"],"ok_pointer":"/ok","value_pointer":"/assistant_text"}},
  {"task_id":"J2","kind":"aggregate","depends_on":["A","B"],
   "aggregate":{"mode":"collect","task_ids":["A","B"],"pointers":["/ok","/assistant_text"],"require_all":True}}
]
print(json.dumps({"tasks": tasks, "allow_inline_api_keys": True}))
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
for _ in $(seq 1 200); do
  final="$(curl -fsS --noproxy "*" --max-time 5 \
    "${DAEMON_URL}/api/v1/workflow?workflow_id=${workflow_id}&include_tasks=1&include_results=1")"
  if python3 - <<PY >/dev/null 2>&1
import json, sys
obj = json.loads(r'''${final}''')
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
obj = json.loads(r'''${final}''')
if not obj.get("ok"):
  print("workflow get failed", obj, file=sys.stderr)
  raise SystemExit(1)
w = obj.get("workflow") or {}
if w.get("status") != "done":
  print("expected workflow status done", w, file=sys.stderr)
  raise SystemExit(1)
res = obj.get("result") or {}
by = res.get("results_by_task") or {}
j1 = by.get("J1") or {}
j2 = by.get("J2") or {}
if j1.get("mode") != "first_ok" or j1.get("ok") is not True:
  print("unexpected J1", j1, file=sys.stderr)
  raise SystemExit(1)
if (j1.get("assistant_text") or "").strip() != "ok":
  print("unexpected J1 assistant_text", j1.get("assistant_text"), file=sys.stderr)
  raise SystemExit(1)
if j1.get("chosen_task_id") != "B":
  print("unexpected chosen_task_id", j1.get("chosen_task_id"), file=sys.stderr)
  raise SystemExit(1)

if j2.get("mode") != "collect" or j2.get("ok") is not True:
  print("unexpected J2", j2, file=sys.stderr)
  raise SystemExit(1)
col = j2.get("collected") or {}
if "/assistant_text" not in col or "/ok" not in col:
  print("missing collected keys", col.keys(), file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_workflow_aggregate_first_ok_smoke OK"
