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

# OpenAI-compatible stub: echo last user prompt as assistant content.
python3 -u - <<PY > "${LOG_DIR}/agentd_workflow_drr_cost_telemetry_smoke.stub.stdout.log" 2> "${LOG_DIR}/agentd_workflow_drr_cost_telemetry_smoke.stub.stderr.log" &
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

# Start agentd with DRR + telemetry-driven cost charging enabled.
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_workflow_drr_cost_telemetry_smoke" \
  --tools none \
  --workflow-fair-queue-policy drr \
  --workflow-drr-cost-model telemetry_v1

agentd_smoke_wait_health "${DAEMON_URL}"

NODE_ID="node_sensor_1"

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(STUB_BASE="${STUB_BASE}" NODE_ID="${NODE_ID}" python3 - <<'PY'
import json, os

stub_base = os.environ["STUB_BASE"]
node_id = os.environ["NODE_ID"]

defaults = {
  "no_session": True,
  "tools": "none",
  "base_url": stub_base,
  "api_key": "dummy",
  "model": "stub",
  "trace": False
}

tasks = [
  {
    "task_id": "W",
    "kind": "edge_wait_sensor",
    "max_attempts": 120,
    "edge_wait_sensor": {
      "event_type": "dirt_detected",
      "node_id": node_id,
      "min_confidence": 0.8,
      "poll_ms": 25
    }
  },
  {
    "task_id": "T",
    "depends_on": ["W"],
    "request": {
      "prompt": "evt=${task.W.json:/edge_sensor_event/event_type} node=${task.W.json:/edge_sensor_event/node_id} conf=${task.W.json:/edge_sensor_event/confidence}"
    },
    "expect": {"assistant_text_contains": "dirt_detected"}
  }
]

print(json.dumps({"tasks": tasks, "defaults": defaults, "allow_inline_api_keys": True}))
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

# Emit a matching SENSOR_EVENT. The workflow wait should observe it and complete.
sensor_msg_id="$(python3 - <<'PY'
import uuid
print(str(uuid.uuid4()))
PY
)"

sensor_evt="$(python3 - <<PY
import json, time
print(json.dumps({
  "msg_id": "${sensor_msg_id}",
  "ts_utc_ms": int(time.time()*1000),
  "type": "SENSOR_EVENT",
  "from": "node:${NODE_ID}",
  "to": "platform",
  "body": {"node_id":"${NODE_ID}","event_type":"dirt_detected","ts_utc_ms": int(time.time()*1000), "confidence": 0.9, "data":{"x":1}},
}))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${sensor_evt}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

final=""
for _ in $(seq 1 240); do
  final="$(curl -fsS --noproxy "*" --max-time 5 \
    "${DAEMON_URL}/api/v1/workflow?workflow_id=${workflow_id}&include_tasks=1&include_results=1")"
  if python3 - <<PY >/dev/null 2>&1
import json
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

ww = by.get("W") or {}
if ww.get("kind") != "edge_wait_sensor" or ww.get("ok") is not True:
  print("expected W ok true edge_wait_sensor", ww, file=sys.stderr)
  raise SystemExit(1)
ev = ww.get("edge_sensor_event") or {}
if ev.get("event_type") != "dirt_detected" or ev.get("node_id") != "${NODE_ID}":
  print("unexpected edge_sensor_event", ev, file=sys.stderr)
  raise SystemExit(1)

t = by.get("T") or {}
if t.get("ok") is not True:
  print("expected T ok true", t, file=sys.stderr)
  raise SystemExit(1)
txt = t.get("assistant_text") or ""
if "dirt_detected" not in txt or "${NODE_ID}" not in txt:
  print("expected T assistant_text to include event_type + node_id; got", txt, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_workflow_drr_cost_telemetry_smoke OK"

