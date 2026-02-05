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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_task_done_result_schema_validation_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

NODE_ID="node_result_schema_1"
CAPS_SHA="sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
TOOL_NAME="ui.led.ws2812.control"

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
      "parameters_schema": {
        "type": "object",
        "additionalProperties": False,
        "properties": {"action": {"type": "string", "enum": ["on", "off"]}},
        "required": ["action"]
      },
      "result_schema": {
        "type": "object",
        "additionalProperties": False,
        "properties": {
          "applied": {
            "type": "object",
            "additionalProperties": False,
            "properties": {"action": {"type": "string"}},
            "required": ["action"]
          }
        },
        "required": ["applied"]
      },
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

TASK_ID_BAD="t_result_bad"
STEP_ID="s"
IDEMPOTENCY_BAD="k_result_bad"

assign_bad="$(python3 - <<PY
import json
print(json.dumps({
  "node_id": "${NODE_ID}",
  "task_id": "${TASK_ID_BAD}",
  "step_id": "${STEP_ID}",
  "idempotency_key": "${IDEMPOTENCY_BAD}",
  "mode": "invoke",
  "deadline_utc_ms": int("${deadline_utc_ms}"),
  "payload": {"tool": "${TOOL_NAME}", "args": {"action": "on"}},
}))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${assign_bad}" \
  "${DAEMON_URL}/api/v1/edge/task/assign" >/dev/null

done_bad="$(python3 - <<PY
import json, uuid, time
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "TASK_DONE",
  "from": f"node:{'${NODE_ID}'}",
  "to": "platform",
  "body": {
    "task_id": "${TASK_ID_BAD}",
    "step_id": "${STEP_ID}",
    "idempotency_key": "${IDEMPOTENCY_BAD}",
    "result": {"ok": True, "data": {}}
  }
}))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${done_bad}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

task_bad="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/task?task_id=${TASK_ID_BAD}&step_id=${STEP_ID}")"

if ! python3 - <<PY
import json, sys
obj = json.loads(r'''${task_bad}''')
t = obj.get("task") or {}
st = t.get("state")
err = t.get("error") or ""
if st != "FAILED":
  print("expected FAILED state, got:", st, file=sys.stderr)
  raise SystemExit(1)
if "result_schema_mismatch" not in err:
  print("expected result_schema_mismatch in error, got:", err, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY
then
  echo "task_bad: ${task_bad}" >&2
  exit 1
fi

TASK_ID_OK="t_result_ok"
IDEMPOTENCY_OK="k_result_ok"

assign_ok="$(python3 - <<PY
import json
print(json.dumps({
  "node_id": "${NODE_ID}",
  "task_id": "${TASK_ID_OK}",
  "step_id": "${STEP_ID}",
  "idempotency_key": "${IDEMPOTENCY_OK}",
  "mode": "invoke",
  "deadline_utc_ms": int("${deadline_utc_ms}"),
  "payload": {"tool": "${TOOL_NAME}", "args": {"action": "off"}},
}))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${assign_ok}" \
  "${DAEMON_URL}/api/v1/edge/task/assign" >/dev/null

done_ok="$(python3 - <<PY
import json, uuid, time
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "TASK_DONE",
  "from": f"node:{'${NODE_ID}'}",
  "to": "platform",
  "body": {
    "task_id": "${TASK_ID_OK}",
    "step_id": "${STEP_ID}",
    "idempotency_key": "${IDEMPOTENCY_OK}",
    "result": {"ok": True, "data": {"applied": {"action": "off"}}}
  }
}))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${done_ok}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

task_ok="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/task?task_id=${TASK_ID_OK}&step_id=${STEP_ID}")"

if ! python3 - <<PY
import json, sys
obj = json.loads(r'''${task_ok}''')
t = obj.get("task") or {}
st = t.get("state")
if st != "SUCCEEDED":
  print("expected SUCCEEDED state, got:", st, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY
then
  echo "task_ok: ${task_ok}" >&2
  exit 1
fi

echo "agentd_edge_task_done_result_schema_validation_smoke OK"

