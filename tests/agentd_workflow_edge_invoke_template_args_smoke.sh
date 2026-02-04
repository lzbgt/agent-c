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
AUTH_TOKEN="agentd_workflow_edge_invoke_template_args_smoke_token"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

# OpenAI-compatible stub: echoes last user prompt as assistant content.
python3 -u - <<PY > "${LOG_DIR}/agentd_workflow_edge_invoke_template_args_smoke.stub.stdout.log" 2> "${LOG_DIR}/agentd_workflow_edge_invoke_template_args_smoke.stub.stderr.log" &
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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_workflow_edge_invoke_template_args_smoke" \
  --auth-token "${AUTH_TOKEN}" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

NODE_ID="node_test_edge_tpl_1"
CAPS_SHA="sha256:test_caps_edge_tpl_1"

hello_json="$(python3 - <<PY
import json, uuid, time
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "NODE_HELLO",
  "from": f"node:{'${NODE_ID}'}",
  "to": "platform",
  "body": {
    "node_id": "${NODE_ID}",
    "model": "esp32sim_stub",
    "fw_git_sha": "deadbeef",
    "caps_sha256": "${CAPS_SHA}",
  }
}))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${AUTH_TOKEN}" \
  -H "Content-Type: application/json" \
  -d "${hello_json}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

caps_rsp_json="$(python3 - <<PY
import json, uuid, time
manifest = {
  "spec_version": "um-acds/0.1",
  "manifest_version": "0.0.1",
  "caps_sha256": "${CAPS_SHA}",
  "node": {"node_id": "${NODE_ID}"},
  "runtime": {"agent_core": {"version": "0.0.0"}},
  "hardware": {"presence": {"ui.led.ws2812": "present"}},
  "tools": [
    {
      "name": "ui.led.ws2812.control",
      "kind": "actuator",
      "description": "Control LED",
      "parameters_schema": {"type":"object","additionalProperties": False, "properties": {"action":{"type":"string"}}, "required":["action"]},
      "timeout_ms": 500,
      "idempotent": False,
      "side_effect_level": "low",
      "hazards": [],
      "rate_limit": {"max_per_minute": 60, "cooldown_ms": 0},
      "tags": ["led"]
    }
  ],
  "tags": ["lab"]
}
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "NODE_CAPS_RSP",
  "from": "node:${NODE_ID}",
  "to": "platform",
  "body": {
    "node_id": "${NODE_ID}",
    "caps_sha256": "${CAPS_SHA}",
    "manifest": manifest
  }
}))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${AUTH_TOKEN}" \
  -H "Content-Type: application/json" \
  -d "${caps_rsp_json}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Authorization: Bearer ${AUTH_TOKEN}" \
  -H "Content-Type: application/json" \
  -d "$(STUB_BASE="${STUB_BASE}" NODE_ID="${NODE_ID}" python3 - <<'PY'
import json
import os

stub_base = os.environ["STUB_BASE"]
node_id = os.environ["NODE_ID"]

req = {"prompt":"solid","no_session":True,"tools":"none","base_url":stub_base,"api_key":"dummy","model":"stub","trace":False}
tmpl = "${task.A.assistant_text}"
tasks = [
  {"task_id":"A","request":req, "max_attempts": 1},
  {"task_id":"E1","kind":"edge_invoke","depends_on":["A"],"max_attempts":200,
   "edge":{"node_id":node_id,"tool":"ui.led.ws2812.control","args":{"action":tmpl},"timeout_ms":5000}}
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

# Wait for TASK_ASSIGN in outbox and validate templated args.action.
outbox=""
for _ in $(seq 1 160); do
  outbox="$(curl -fsS --noproxy "*" --max-time 10 \
    -H "Authorization: Bearer ${AUTH_TOKEN}" \
    "${DAEMON_URL}/api/v1/edge/outbox?node_id=${NODE_ID}&cursor=0&limit=50")" || true
  if python3 - <<PY >/dev/null 2>&1
import json, sys
obj = json.loads(r'''${outbox}''') if r'''${outbox}''' else {}
msgs = obj.get("messages") or []
for m in msgs:
  env = (m.get("msg") or {})
  if env.get("type") != "TASK_ASSIGN":
    continue
  body = env.get("body") or {}
  if body.get("task_id") != "${workflow_id}":
    continue
  if body.get("step_id") != "E1":
    continue
  payload = body.get("payload") or {}
  args = (payload.get("args") or {})
  if args.get("action") != "solid":
    raise SystemExit(1)
  raise SystemExit(0)
raise SystemExit(1)
PY
  then
    break
  fi
  sleep 0.05
done

# Complete the edge task.
curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${AUTH_TOKEN}" \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json, uuid, time
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "TASK_DONE",
  "from": "node:${NODE_ID}",
  "to": "platform",
  "body": {"task_id":"${workflow_id}","step_id":"E1","result":{"ok":True,"data":{"applied":{"action":"solid"}}}},
}))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

final=""
for _ in $(seq 1 240); do
  final="$(curl -fsS --noproxy "*" --max-time 5 \
    -H "Authorization: Bearer ${AUTH_TOKEN}" \
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
if w.get("status") != "done":
  print("expected workflow status done", w, file=sys.stderr)
  raise SystemExit(1)
res = obj.get("result") or {}
by_task = res.get("results_by_task") or {}
r = by_task.get("A") or {}
if (r.get("assistant_text") or "").strip() != "solid":
  print("expected A assistant_text solid", r, file=sys.stderr)
  raise SystemExit(1)
e = by_task.get("E1") or {}
if e.get("kind") != "edge_invoke" or e.get("ok") is not True:
  print("expected E1 ok edge_invoke", e, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_workflow_edge_invoke_template_args_smoke OK"
