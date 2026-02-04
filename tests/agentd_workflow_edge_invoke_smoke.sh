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
HOST="127.0.0.1"
AUTH_TOKEN="agentd_workflow_edge_invoke_smoke_token"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_workflow_edge_invoke_smoke" \
  --auth-token "${AUTH_TOKEN}" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

NODE_ID="node_test_edge_1"
CAPS_SHA="sha256:test_caps_edge_1"

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
  -d "$(python3 - <<PY
import json
tasks = [
  {"task_id":"E1","kind":"edge_invoke","max_attempts":200,
   "edge":{"node_id":"${NODE_ID}","tool":"ui.led.ws2812.control","args":{"action":"solid"},"timeout_ms":5000}}
]
print(json.dumps({"tasks": tasks}))
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

# Wait for TASK_ASSIGN in outbox and validate payload fields.
outbox=""
for _ in $(seq 1 120); do
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
  if payload.get("tool") != "ui.led.ws2812.control":
    raise SystemExit(1)
  args = payload.get("args") or {}
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

# Node acknowledges and completes the task.
curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${AUTH_TOKEN}" \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json, uuid, time
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "TASK_ACK",
  "from": "node:${NODE_ID}",
  "to": "platform",
  "body": {"task_id":"${workflow_id}","step_id":"E1","accepted":True},
}))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

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

result = obj.get("result") or {}
by_task = result.get("results_by_task") or {}
r = by_task.get("E1") or {}
if r.get("kind") != "edge_invoke":
  print("unexpected E1 kind", r.get("kind"), file=sys.stderr)
  raise SystemExit(1)
if r.get("ok") is not True:
  print("expected ok true", r, file=sys.stderr)
  raise SystemExit(1)
if r.get("edge_state") != "SUCCEEDED":
  print("expected SUCCEEDED", r, file=sys.stderr)
  raise SystemExit(1)
edge_result = r.get("edge_result") or {}
if not edge_result.get("ok"):
  print("expected edge_result.ok true", edge_result, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_workflow_edge_invoke_smoke OK"

