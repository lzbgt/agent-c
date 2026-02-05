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

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_rules_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

NODE_ID="node_rule_1"
CAPS_SHA="sha256:1111111111111111111111111111111111111111111111111111111111111111"

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
  -H "Content-Type: application/json" \
  -d "${hello_json}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

# Provide node capabilities so platform has tool metadata for safety/rate checks.
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
      "hazards": []
    }
  ],
  "safety": {},
  "tags": ["room:lobby"]
}
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "NODE_CAPS_RSP",
  "from": f"node:{'${NODE_ID}'}",
  "to": "platform",
  "body": {"node_id": "${NODE_ID}", "manifest": manifest},
}))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${caps_rsp_json}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

# Create an automation rule: SENSOR_EVENT -> TASK_ASSIGN (invoke tool).
rule_upsert="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<'PY'
import json
print(json.dumps({
  "event_type": "dirt_detected",
  "enabled": True,
  "min_confidence": 0.8,
  "cooldown_ms": 0,
  "action": {
    "type": "task_assign",
    "mode": "invoke",
    "deadline_in_ms": 60_000,
    "target": {"node_id": "node_rule_1"},
    "payload": {"tool":"ui.led.ws2812.control","args":{"action":"blink"}}
  }
}))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/rule/upsert")"

RULE_ID="$(python3 - <<PY
import json
obj = json.loads(r'''${rule_upsert}''')
if not obj.get("ok"):
  raise SystemExit(1)
print(obj.get("rule_id") or "")
PY
)"

if [[ -z "${RULE_ID}" ]]; then
  echo "missing rule_id in response: ${rule_upsert}" >&2
  exit 1
fi

# Trigger a SENSOR_EVENT and assert a TASK_ASSIGN appears in outbox.
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

outbox="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/outbox?node_id=${NODE_ID}&cursor=0&limit=200")"

task_pair="$(python3 - <<PY
import json
obj = json.loads(r'''${outbox}''')
msgs = obj.get("messages") or []
for m in msgs:
  env = m.get("msg") or {}
  if env.get("type") != "TASK_ASSIGN":
    continue
  body = env.get("body") or {}
  payload = body.get("payload") or {}
  if payload.get("tool") != "ui.led.ws2812.control":
    continue
  print(body.get("task_id") or "")
  print(body.get("step_id") or "")
  print(body.get("idempotency_key") or "")
  raise SystemExit(0)
raise SystemExit(1)
PY
)" || {
  echo "outbox: ${outbox}" >&2
  exit 1
}

TASK_ID="$(echo "${task_pair}" | sed -n '1p')"
STEP_ID="$(echo "${task_pair}" | sed -n '2p')"
IDEM_KEY="$(echo "${task_pair}" | sed -n '3p')"
if [[ -z "${TASK_ID}" || -z "${STEP_ID}" || -z "${IDEM_KEY}" ]]; then
  echo "failed to extract task_id/step_id from outbox: ${outbox}" >&2
  exit 1
fi

# Complete the task and verify platform records SUCCEEDED.
curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json, uuid, time
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "TASK_DONE",
  "from": "node:${NODE_ID}",
  "to": "platform",
  "body": {"task_id":"${TASK_ID}","step_id":"${STEP_ID}","idempotency_key":"${IDEM_KEY}","result":{"ok":True,"data":{"rule":"${RULE_ID}"}}},
}))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

task_get="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/task?task_id=${TASK_ID}&step_id=${STEP_ID}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${task_get}''')
if not obj.get("ok"):
  print("task_get not ok:", obj, file=sys.stderr)
  raise SystemExit(1)
t = obj.get("task") or {}
if t.get("state") != "SUCCEEDED":
  print("expected SUCCEEDED, got:", t.get("state"), t, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY
