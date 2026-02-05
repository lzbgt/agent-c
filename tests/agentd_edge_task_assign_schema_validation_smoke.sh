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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_task_assign_schema_validation_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

NODE_ID="node_schema_1"
CAPS_SHA="sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
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

outbox_0="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/outbox?node_id=${NODE_ID}&cursor=0&limit=50")"

if ! python3 - <<PY
import json, sys
obj = json.loads(r'''${outbox_0}''')
msgs = obj.get("messages") or []
types = []
for m in msgs:
  env = (m.get("msg") or {})
  t = env.get("type")
  if isinstance(t, str):
    types.append(t)
if "PLATFORM_CAPS_REQ" not in types:
  print("expected PLATFORM_CAPS_REQ in outbox, got:", types, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY
then
  echo "outbox_0: ${outbox_0}" >&2
  exit 1
fi

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
        "properties": {
          "action": {"type": "string", "enum": ["on", "off"]},
          "brightness": {"type": "integer"}
        },
        "required": ["action"]
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

assign_bad_type="$(python3 - <<PY
import json, uuid
print(json.dumps({
  "node_id": "${NODE_ID}",
  "task_id": "t_bad_type_" + uuid.uuid4().hex[:8],
  "step_id": "s",
  "idempotency_key": "k_bad_type_" + uuid.uuid4().hex[:8],
  "mode": "invoke",
  "deadline_utc_ms": int("${deadline_utc_ms}"),
  "payload": {"tool": "${TOOL_NAME}", "args": "not_an_object"},
}))
PY
)"

status_bad_type="$(curl -sS --noproxy "*" --max-time 10 \
  -o "${LOG_DIR}/agentd_edge_task_assign_schema_validation_smoke.bad_type.body.json" \
  -w "%{http_code}" \
  -H "Content-Type: application/json" \
  -d "${assign_bad_type}" \
  "${DAEMON_URL}/api/v1/edge/task/assign")"

if [[ "${status_bad_type}" != "400" ]]; then
  echo "expected bad args type to return 400, got ${status_bad_type}" >&2
  cat "${LOG_DIR}/agentd_edge_task_assign_schema_validation_smoke.bad_type.body.json" >&2 || true
  exit 1
fi

assign_missing_required="$(python3 - <<PY
import json, uuid
print(json.dumps({
  "node_id": "${NODE_ID}",
  "task_id": "t_missing_req_" + uuid.uuid4().hex[:8],
  "step_id": "s",
  "idempotency_key": "k_missing_req_" + uuid.uuid4().hex[:8],
  "mode": "invoke",
  "deadline_utc_ms": int("${deadline_utc_ms}"),
  "payload": {"tool": "${TOOL_NAME}", "args": {}},
}))
PY
)"

status_missing_required="$(curl -sS --noproxy "*" --max-time 10 \
  -o "${LOG_DIR}/agentd_edge_task_assign_schema_validation_smoke.missing_required.body.json" \
  -w "%{http_code}" \
  -H "Content-Type: application/json" \
  -d "${assign_missing_required}" \
  "${DAEMON_URL}/api/v1/edge/task/assign")"

if [[ "${status_missing_required}" != "400" ]]; then
  echo "expected missing required property to return 400, got ${status_missing_required}" >&2
  cat "${LOG_DIR}/agentd_edge_task_assign_schema_validation_smoke.missing_required.body.json" >&2 || true
  exit 1
fi

assign_unknown_prop="$(python3 - <<PY
import json, uuid
print(json.dumps({
  "node_id": "${NODE_ID}",
  "task_id": "t_unknown_prop_" + uuid.uuid4().hex[:8],
  "step_id": "s",
  "idempotency_key": "k_unknown_prop_" + uuid.uuid4().hex[:8],
  "mode": "invoke",
  "deadline_utc_ms": int("${deadline_utc_ms}"),
  "payload": {"tool": "${TOOL_NAME}", "args": {"action":"on","nope":123}},
}))
PY
)"

status_unknown_prop="$(curl -sS --noproxy "*" --max-time 10 \
  -o "${LOG_DIR}/agentd_edge_task_assign_schema_validation_smoke.unknown_prop.body.json" \
  -w "%{http_code}" \
  -H "Content-Type: application/json" \
  -d "${assign_unknown_prop}" \
  "${DAEMON_URL}/api/v1/edge/task/assign")"

if [[ "${status_unknown_prop}" != "400" ]]; then
  echo "expected unknown property to return 400, got ${status_unknown_prop}" >&2
  cat "${LOG_DIR}/agentd_edge_task_assign_schema_validation_smoke.unknown_prop.body.json" >&2 || true
  exit 1
fi

assign_ok="$(python3 - <<PY
import json, uuid
print(json.dumps({
  "node_id": "${NODE_ID}",
  "task_id": "t_ok_" + uuid.uuid4().hex[:8],
  "step_id": "s",
  "idempotency_key": "k_ok_" + uuid.uuid4().hex[:8],
  "mode": "invoke",
  "deadline_utc_ms": int("${deadline_utc_ms}"),
  "payload": {"tool": "${TOOL_NAME}", "args": {"action":"on","brightness": 10}},
}))
PY
)"

status_ok="$(curl -sS --noproxy "*" --max-time 10 \
  -o "${LOG_DIR}/agentd_edge_task_assign_schema_validation_smoke.ok.body.json" \
  -w "%{http_code}" \
  -H "Content-Type: application/json" \
  -d "${assign_ok}" \
  "${DAEMON_URL}/api/v1/edge/task/assign")"

if [[ "${status_ok}" != "200" ]]; then
  echo "expected valid args to return 200, got ${status_ok}" >&2
  cat "${LOG_DIR}/agentd_edge_task_assign_schema_validation_smoke.ok.body.json" >&2 || true
  exit 1
fi

echo "agentd_edge_task_assign_schema_validation_smoke OK"

