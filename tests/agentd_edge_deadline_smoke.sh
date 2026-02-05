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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_deadline_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

NODE_ID="node_deadline_1"
CAPS_SHA="sha256:test_caps_deadline_1"

# Register node + caps so platform allows mode=invoke (manifest required).
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
curl -fsS --noproxy "*" --max-time 10 -H "Content-Type: application/json" -d "${hello_json}" \
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
      "hazards": []
    }
  ],
  "safety": {},
  "tags": []
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
curl -fsS --noproxy "*" --max-time 10 -H "Content-Type: application/json" -d "${caps_rsp_json}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

deadline="$(python3 - <<'PY'
import time
print(int(time.time()*1000) + 1000)
PY
)"

assign_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "node_id": "${NODE_ID}",
  "task_id": "task_deadline_1",
  "step_id": "s1",
  "idempotency_key": "idem_deadline_1",
  "mode": "invoke",
  "deadline_utc_ms": int("${deadline}"),
  "payload": {"tool":"ui.led.ws2812.control","args":{"action":"solid"}}
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
print("ok")
PY

# Wait for sweeper to mark it TIMED_OUT.
for _ in $(seq 1 80); do
  task_get="$(curl -fsS --noproxy "*" --max-time 10 \
    "${DAEMON_URL}/api/v1/edge/task?task_id=task_deadline_1&step_id=s1")"
  st="$(python3 - <<PY
import json
obj = json.loads(r'''${task_get}''')
t = obj.get("task") or {}
print(t.get("state") or "")
PY
)"
  if [[ "${st}" == "TIMED_OUT" ]]; then
    break
  fi
  sleep 0.1
done

task_get="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/task?task_id=task_deadline_1&step_id=s1")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${task_get}''')
t = obj.get("task") or {}
if t.get("state") != "TIMED_OUT":
  print("expected TIMED_OUT, got:", t.get("state"), t, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

# Late completion should not override platform TIMED_OUT.
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
  "body": {"task_id":"task_deadline_1","step_id":"s1","idempotency_key":"idem_deadline_1","result":{"ok":True,"data":{"late":True}}},
}))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

task_get2="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/task?task_id=task_deadline_1&step_id=s1")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${task_get2}''')
t = obj.get("task") or {}
if t.get("state") != "TIMED_OUT":
  print("expected TIMED_OUT after late done, got:", t.get("state"), t, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY
