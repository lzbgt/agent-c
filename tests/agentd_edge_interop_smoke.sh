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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_interop_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

NODE_ID="node_test_1"
CAPS_SHA="sha256:test_caps_1"

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

caps_get="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/node/caps?node_id=${NODE_ID}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${caps_get}''')
if not obj.get("ok"):
  print("caps_get not ok:", obj, file=sys.stderr)
  raise SystemExit(1)
m = obj.get("manifest") or {}
tools = m.get("tools") or []
names = [t.get("name") for t in tools if isinstance(t, dict)]
if "ui.led.ws2812.control" not in names:
  print("expected tool in manifest, got:", names, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

deadline="$(python3 - <<'PY'
import time
print(int(time.time()*1000) + 60_000)
PY
)"

assign_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "match_any": {"requires_tools": ["ui.led.ws2812.control"], "tags_all": ["room:lobby"]},
  "task_id": "task_1",
  "step_id": "step_led",
  "idempotency_key": "idem_1",
  "mode": "invoke",
  "deadline_utc_ms": int("${deadline}"),
  "payload": {"tool": "ui.led.ws2812.control", "args": {"action": "solid"}}
}))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/task/assign")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${assign_resp}''')
if not obj.get("ok"):
  print("assign not ok:", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("node_id") != "${NODE_ID}":
  print("assign chose wrong node:", obj, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

# Node acknowledges and completes the task.
curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json, uuid, time
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "TASK_ACK",
  "from": "node:${NODE_ID}",
  "to": "platform",
  "body": {"task_id":"task_1","step_id":"step_led","accepted":True},
}))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json, uuid, time
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "TASK_EVENT",
  "from": "node:${NODE_ID}",
  "to": "platform",
  "body": {"task_id":"task_1","step_id":"step_led","state":"RUNNING","progress":0.5},
}))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

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
  "body": {"task_id":"task_1","step_id":"step_led","result":{"ok":True,"data":{"applied":{"action":"solid"}}}},
}))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

task_get="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/task?task_id=task_1&step_id=step_led")"

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
res = t.get("result") or {}
if not isinstance(res, dict) or not res.get("ok"):
  print("expected ok result, got:", res, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY
