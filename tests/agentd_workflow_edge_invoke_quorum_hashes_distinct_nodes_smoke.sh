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
AUTH_TOKEN="agentd_workflow_edge_invoke_quorum_hashes_distinct_nodes_smoke_token"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_workflow_edge_invoke_quorum_hashes_distinct_nodes_smoke" \
  --auth-token "${AUTH_TOKEN}" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

NODE_A="node_test_edge_A"
CAPS_SHA="sha256:4444444444444444444444444444444444444444444444444444444444444444"

send_edge_msg() {
  local json="${1:?}"
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Authorization: Bearer ${AUTH_TOKEN}" \
    -H "Content-Type: application/json" \
    -d "${json}" \
    "${DAEMON_URL}/api/v1/edge/message" >/dev/null
}

node_hello() {
  local node_id="${1:?}"
  python3 - <<PY
import json, uuid, time
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "NODE_HELLO",
  "from": f"node:{'${node_id}'}",
  "to": "platform",
  "body": {
    "node_id": "${node_id}",
    "model": "esp32sim_stub",
    "fw_git_sha": "deadbeef",
    "caps_sha256": "${CAPS_SHA}",
  }
}))
PY
}

node_caps_rsp() {
  local node_id="${1:?}"
  python3 - <<PY
import json, uuid, time
manifest = {
  "spec_version": "um-acds/0.1",
  "manifest_version": "0.0.1",
  "caps_sha256": "${CAPS_SHA}",
  "node": {"node_id": "${node_id}"},
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
  "from": "node:${node_id}",
  "to": "platform",
  "body": {
    "node_id": "${node_id}",
    "caps_sha256": "${CAPS_SHA}",
    "manifest": manifest
  }
}))
PY
}

send_edge_msg "$(node_hello "${NODE_A}")"
send_edge_msg "$(node_caps_rsp "${NODE_A}")"

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Authorization: Bearer ${AUTH_TOKEN}" \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
tasks = [
  {"task_id":"E1","kind":"edge_invoke","max_attempts":200,
   "edge":{"node_id":"${NODE_A}","tool":"ui.led.ws2812.control","args":{"action":"solid"},"timeout_ms":5000}},
  {"task_id":"E2","kind":"edge_invoke","max_attempts":200,
   "edge":{"node_id":"${NODE_A}","tool":"ui.led.ws2812.control","args":{"action":"solid"},"timeout_ms":5000}},
  {"task_id":"J","kind":"aggregate","allow_error":True,"depends_on":["E1","E2"],
   "aggregate":{"mode":"quorum_hashes","task_ids":["E1","E2"],"quorum":2,"pointers":["/edge_result_sha256"],"node_pointer":"/edge/node_id","require_distinct_nodes":True}},
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

wait_outbox_assign() {
  local node_id="${1:?}"
  local step_id="${2:?}"
  for _ in $(seq 1 120); do
    local outbox
    outbox="$(curl -fsS --noproxy "*" --max-time 10 \
      -H "Authorization: Bearer ${AUTH_TOKEN}" \
      "${DAEMON_URL}/api/v1/edge/outbox?node_id=${node_id}&cursor=0&limit=50")" || true
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
  if body.get("step_id") != "${step_id}":
    continue
  if body.get("idempotency_key") != "${workflow_id}:${step_id}":
    raise SystemExit(1)
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
      return 0
    fi
    sleep 0.05
  done
  echo "timed out waiting for TASK_ASSIGN for ${node_id}/${step_id}" >&2
  return 1
}

wait_outbox_assign "${NODE_A}" "E1"
wait_outbox_assign "${NODE_A}" "E2"

task_ack() {
  local node_id="${1:?}"
  local step_id="${2:?}"
  python3 - <<PY
import json, uuid, time
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "TASK_ACK",
  "from": "node:${node_id}",
  "to": "platform",
  "body": {"task_id":"${workflow_id}","step_id":"${step_id}","idempotency_key":"${workflow_id}:${step_id}","accepted":True},
}))
PY
}

task_done() {
  local node_id="${1:?}"
  local step_id="${2:?}"
  python3 - <<PY
import json, uuid, time
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "TASK_DONE",
  "from": "node:${node_id}",
  "to": "platform",
  "body": {
    "task_id":"${workflow_id}",
    "step_id":"${step_id}",
    "idempotency_key":"${workflow_id}:${step_id}",
    "result":{"ok":True,"data":{"applied":{"action":"solid"}},"attest":{"result_sha256":"sha256:" + ("0"*64), "kid":"test-kid", "sig":"test-sig"}}
  },
}))
PY
}

send_edge_msg "$(task_ack "${NODE_A}" "E1")"
send_edge_msg "$(task_done "${NODE_A}" "E1")"
send_edge_msg "$(task_ack "${NODE_A}" "E2")"
send_edge_msg "$(task_done "${NODE_A}" "E2")"

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
j = by_task.get("J") or {}
if j.get("kind") != "aggregate" or j.get("mode") != "quorum_hashes" or j.get("ok") is not False:
  print("expected J ok=false quorum_hashes", j, file=sys.stderr)
  raise SystemExit(1)
nodes = j.get("nodes_by_task_id") or {}
if nodes.get("E1") != "${NODE_A}" or nodes.get("E2") != "${NODE_A}":
  print("unexpected nodes_by_task_id", nodes, file=sys.stderr)
  raise SystemExit(1)
checks = j.get("checks") or []
if not checks or checks[0].get("ptr") != "/edge_result_sha256" or checks[0].get("ok") is not False:
  print("unexpected checks", checks, file=sys.stderr)
  raise SystemExit(1)
if checks[0].get("require_distinct_nodes") is not True or checks[0].get("count_kind") != "nodes":
  print("expected distinct-node counting", checks[0], file=sys.stderr)
  raise SystemExit(1)
if checks[0].get("chosen_count") != 1 or checks[0].get("quorum") != 2:
  print("expected chosen_count=1 quorum=2", checks[0], file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_workflow_edge_invoke_quorum_hashes_distinct_nodes_smoke OK"

