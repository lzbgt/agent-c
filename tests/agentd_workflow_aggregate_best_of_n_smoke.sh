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
# - echoes the last user prompt as assistant content
python3 -u - <<PY > "${LOG_DIR}/agentd_workflow_aggregate_best_of_n_smoke.stub.stdout.log" 2> "${LOG_DIR}/agentd_workflow_aggregate_best_of_n_smoke.stub.stderr.log" &
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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_workflow_aggregate_best_of_n_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
req_a = {"prompt": "{\"score\": 1, \"answer\": \"alpha\"}", "no_session": True, "tools": "none", "base_url": "${STUB_BASE}", "api_key": "dummy", "model": "stub", "trace": False}
req_b = {"prompt": "{\"score\": 9, \"answer\": \"bravo\"}", "no_session": True, "tools": "none", "base_url": "${STUB_BASE}", "api_key": "dummy", "model": "stub", "trace": False}
req_c = {"prompt": "{\"score\": 5, \"answer\": \"charlie\"}", "no_session": True, "tools": "none", "base_url": "${STUB_BASE}", "api_key": "dummy", "model": "stub", "trace": False}
tasks = [
  {"task_id":"A","request":req_a, "max_attempts": 1},
  {"task_id":"B","request":req_b, "max_attempts": 1},
  {"task_id":"C","request":req_c, "max_attempts": 1},
  {"task_id":"J","kind":"aggregate","depends_on":["A","B","C"],
   "aggregate":{"mode":"best_of_n","task_ids":["A","B","C"],
                "candidate_pointer":"/assistant_text","parse_json":True,
                "score_pointer":"/score","value_pointer":"/answer","maximize":True}}
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
j = by.get("J") or {}
if j.get("mode") != "best_of_n" or j.get("ok") is not True:
  print("unexpected J", j, file=sys.stderr)
  raise SystemExit(1)
if j.get("chosen_task_id") != "B":
  print("unexpected chosen_task_id", j.get("chosen_task_id"), file=sys.stderr)
  raise SystemExit(1)
if (j.get("assistant_text") or "").strip() != "bravo":
  print("unexpected assistant_text", j.get("assistant_text"), file=sys.stderr)
  raise SystemExit(1)
if float(j.get("chosen_score", -1)) != 9.0:
  print("unexpected chosen_score", j.get("chosen_score"), file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_workflow_aggregate_best_of_n_smoke OK"

