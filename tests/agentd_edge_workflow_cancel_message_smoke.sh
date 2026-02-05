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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_workflow_cancel_message_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

NODE_ID="node_wf_cancel_msg_1"
CAPS_SHA="sha256:test_caps_wf_cancel_msg_1"

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

WF_ID="$(python3 - <<'PY'
import uuid
print("wf_" + str(uuid.uuid4()))
PY
)"

deadline="$(python3 - <<'PY'
import time
print(int(time.time()*1000) + 60_000)
PY
)"

wf_submit_msg="$(python3 - <<PY
import json, uuid, time
workflow = {
  "workflow_id": "${WF_ID}",
  "goal": "submit via WORKFLOW_SUBMIT then cancel via WORKFLOW_CANCEL",
  "priority": 1,
  "steps": [
    {
      "step_id": "a",
      "kind": "invoke_tool",
      "depends_on": [],
      "target": {"node_id": "${NODE_ID}"},
      "payload": {"tool":"ui.led.ws2812.control","args":{"action":"blink"}},
      "deadline_utc_ms": int("${deadline}"),
      "max_attempts": 1
    }
  ]
}
env = {
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "WORKFLOW_SUBMIT",
  "from": "node:${NODE_ID}",
  "to": "platform",
  "body": {"workflow": workflow},
}
print(json.dumps(env))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${wf_submit_msg}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

# Wait until a WORKFLOW_ACK (submit) is visible in outbox.
for _ in $(seq 1 80); do
  outbox="$(curl -fsS --noproxy "*" --max-time 10 \
    "${DAEMON_URL}/api/v1/edge/outbox?node_id=${NODE_ID}&cursor=0&limit=400")"
  ok="$(python3 - <<PY
import json
obj = json.loads(r'''${outbox}''')
msgs = obj.get("messages") or []
for m in msgs:
  env = m.get("msg") or {}
  if env.get("type") != "WORKFLOW_ACK":
    continue
  body = env.get("body") or {}
  if body.get("workflow_id") == "${WF_ID}" and body.get("ok") is True:
    print("1")
    break
else:
  print("0")
PY
)"
  if [[ "${ok}" == "1" ]]; then
    break
  fi
  sleep 0.05
done

wf_cancel_msg="$(python3 - <<PY
import json, uuid, time
env = {
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "WORKFLOW_CANCEL",
  "from": "node:${NODE_ID}",
  "to": "platform",
  "body": {"workflow_id": "${WF_ID}"},
}
print(json.dumps(env))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${wf_cancel_msg}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

# Expect a best-effort cancel ACK in outbox.
for _ in $(seq 1 80); do
  outbox="$(curl -fsS --noproxy "*" --max-time 10 \
    "${DAEMON_URL}/api/v1/edge/outbox?node_id=${NODE_ID}&cursor=0&limit=600")"
  ok="$(python3 - <<PY
import json
obj = json.loads(r'''${outbox}''')
msgs = obj.get("messages") or []
for m in msgs:
  env = m.get("msg") or {}
  if env.get("type") != "WORKFLOW_ACK":
    continue
  body = env.get("body") or {}
  if body.get("workflow_id") != "${WF_ID}":
    continue
  if body.get("ok") is True and body.get("status") == "CANCELED":
    print("1")
    break
else:
  print("0")
PY
)"
  if [[ "${ok}" == "1" ]]; then
    break
  fi
  sleep 0.05
done

# Poll workflow status until it becomes CANCELED.
final=""
for _ in $(seq 1 120); do
  final="$(curl -fsS --noproxy "*" --max-time 10 \
    "${DAEMON_URL}/api/v1/edge/workflow?workflow_id=${WF_ID}&include_steps=1")"
  if python3 - <<PY >/dev/null 2>&1
import json, sys
obj = json.loads(r'''${final}''')
wf = obj.get("workflow") or {}
raise SystemExit(0 if wf.get("status") == "CANCELED" else 1)
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
wf = obj.get("workflow") or {}
if wf.get("status") != "CANCELED":
  print("expected workflow status CANCELED", wf, file=sys.stderr)
  raise SystemExit(1)
PY

