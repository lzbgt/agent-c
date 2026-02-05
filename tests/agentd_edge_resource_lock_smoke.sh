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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_resource_lock_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

NODE_ID="node_lock_1"
CAPS_SHA="sha256:babababababababababababababababababababababababababababababababa"
TOOL_NAME="ui.led.ws2812.control"
LOCK_KEY="ui.led"

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
      "name": "${TOOL_NAME}",
      "kind": "actuator",
      "description": "Control LED",
      "resource_lock": "${LOCK_KEY}",
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

deadline_utc_ms="$(python3 - <<PY
import time
print(int(time.time()*1000) + 60000)
PY
)"

TASK1="t_lock_1"
STEP="s"
KEY1="k_lock_1"

assign1="$(python3 - <<PY
import json
print(json.dumps({
  "node_id": "${NODE_ID}",
  "task_id": "${TASK1}",
  "step_id": "${STEP}",
  "idempotency_key": "${KEY1}",
  "mode": "invoke",
  "deadline_utc_ms": int("${deadline_utc_ms}"),
  "payload": {"tool": "${TOOL_NAME}", "args": {"action": "on"}},
}))
PY
)"

status_1="$(curl -sS --noproxy "*" --max-time 10 \
  -o "${LOG_DIR}/agentd_edge_resource_lock_smoke.assign1.body.json" \
  -w "%{http_code}" \
  -H "Content-Type: application/json" \
  -d "${assign1}" \
  "${DAEMON_URL}/api/v1/edge/task/assign")"

if [[ "${status_1}" != "200" ]]; then
  echo "expected first assign to return 200, got ${status_1}" >&2
  cat "${LOG_DIR}/agentd_edge_resource_lock_smoke.assign1.body.json" >&2 || true
  exit 1
fi

TASK2="t_lock_2"
KEY2="k_lock_2"

assign2="$(python3 - <<PY
import json
print(json.dumps({
  "node_id": "${NODE_ID}",
  "task_id": "${TASK2}",
  "step_id": "${STEP}",
  "idempotency_key": "${KEY2}",
  "mode": "invoke",
  "deadline_utc_ms": int("${deadline_utc_ms}"),
  "payload": {"tool": "${TOOL_NAME}", "args": {"action": "off"}},
}))
PY
)"

status_2="$(curl -sS --noproxy "*" --max-time 10 \
  -o "${LOG_DIR}/agentd_edge_resource_lock_smoke.assign2.body.json" \
  -w "%{http_code}" \
  -H "Content-Type: application/json" \
  -d "${assign2}" \
  "${DAEMON_URL}/api/v1/edge/task/assign")"

if [[ "${status_2}" != "429" ]]; then
  echo "expected second assign to return 429 due to resource_lock, got ${status_2}" >&2
  cat "${LOG_DIR}/agentd_edge_resource_lock_smoke.assign2.body.json" >&2 || true
  exit 1
fi

done1="$(python3 - <<PY
import json, uuid, time
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "TASK_DONE",
  "from": f"node:{'${NODE_ID}'}",
  "to": "platform",
  "body": {
    "task_id": "${TASK1}",
    "step_id": "${STEP}",
    "idempotency_key": "${KEY1}",
    "result": {"ok": True, "data": {"applied": True}}
  }
}))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${done1}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

status_3="$(curl -sS --noproxy "*" --max-time 10 \
  -o "${LOG_DIR}/agentd_edge_resource_lock_smoke.assign2_retry.body.json" \
  -w "%{http_code}" \
  -H "Content-Type: application/json" \
  -d "${assign2}" \
  "${DAEMON_URL}/api/v1/edge/task/assign")"

if [[ "${status_3}" != "200" ]]; then
  echo "expected assign after release to return 200, got ${status_3}" >&2
  cat "${LOG_DIR}/agentd_edge_resource_lock_smoke.assign2_retry.body.json" >&2 || true
  exit 1
fi

task_get="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/task?task_id=${TASK1}&step_id=${STEP}")"

if ! python3 - <<PY
import json, sys
obj=json.loads(r'''${task_get}''')
t=obj.get("task") or {}
if t.get("resource_lock") != "${LOCK_KEY}":
  print("expected resource_lock on task record", t.get("resource_lock"), file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY
then
  echo "task_get: ${task_get}" >&2
  exit 1
fi

echo "agentd_edge_resource_lock_smoke OK"

