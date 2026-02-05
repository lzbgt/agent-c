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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_workflow_submit_message_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

NODE_ID="node_wf_msg_1"
CAPS_SHA="sha256:4444444444444444444444444444444444444444444444444444444444444444"

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
  "goal": "submit via WORKFLOW_SUBMIT message",
  "priority": 1,
  "steps": [
    {
      "step_id": "a",
      "kind": "invoke_tool",
      "depends_on": [],
      "target": {"node_id": "${NODE_ID}"},
      "payload": {"tool":"ui.led.ws2812.control","args":{"action":"blink"}},
      "deadline_utc_ms": int("${deadline}")
    },
    {
      "step_id": "b",
      "kind": "invoke_tool",
      "depends_on": [],
      "target": {"node_id": "${NODE_ID}"},
      "payload": {"tool":"ui.led.ws2812.control","args":{"action":"solid"}},
      "deadline_utc_ms": int("${deadline}")
    },
    {
      "step_id": "join_all",
      "kind": "join",
      "depends_on": ["a","b"],
      "join_mode": "all",
      "target": {},
      "payload": {}
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

# Expect a best-effort WORKFLOW_ACK in outbox.
for _ in $(seq 1 60); do
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
  sleep 0.1
done

# Wait until both TASK_ASSIGN messages appear in outbox.
for _ in $(seq 1 80); do
  outbox="$(curl -fsS --noproxy "*" --max-time 10 \
    "${DAEMON_URL}/api/v1/edge/outbox?node_id=${NODE_ID}&cursor=0&limit=400")"
  found="$(python3 - <<PY
import json
obj = json.loads(r'''${outbox}''')
msgs = obj.get("messages") or []
seen = set()
for m in msgs:
  env = m.get("msg") or {}
  if env.get("type") != "TASK_ASSIGN":
    continue
  body = env.get("body") or {}
  if body.get("task_id") != "${WF_ID}":
    continue
  sid = body.get("step_id")
  if sid in ("a","b"):
    seen.add(sid)
print(",".join(sorted(seen)))
PY
)"
  if [[ "${found}" == "a,b" ]]; then
    break
  fi
  sleep 0.1
done

# Extract per-step idempotency_key from the TASK_ASSIGN envelopes (required for task lifecycle correlation).
idem_pair="$(python3 - <<PY
import json
obj = json.loads(r'''${outbox}''')
msgs = obj.get("messages") or []
idem = {}
for m in msgs:
  env = m.get("msg") or {}
  if env.get("type") != "TASK_ASSIGN":
    continue
  body = env.get("body") or {}
  if body.get("task_id") != "${WF_ID}":
    continue
  sid = body.get("step_id")
  if sid in ("a","b"):
    idem[sid] = body.get("idempotency_key") or ""
print(idem.get("a",""))
print(idem.get("b",""))
PY
)"
IDEM_A="$(echo "${idem_pair}" | sed -n '1p')"
IDEM_B="$(echo "${idem_pair}" | sed -n '2p')"
if [[ -z "${IDEM_A}" || -z "${IDEM_B}" ]]; then
  echo "failed to extract idempotency keys from outbox: ${outbox}" >&2
  exit 1
fi

# Complete both tasks.
for STEP in a b; do
  IDEM="${IDEM_A}"
  if [[ "${STEP}" == "b" ]]; then IDEM="${IDEM_B}"; fi
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
  "body": {"task_id":"${WF_ID}","step_id":"${STEP}","idempotency_key":"${IDEM}","result":{"ok":True,"data":{"step":"${STEP}"}}},
}))
PY
)" \
    "${DAEMON_URL}/api/v1/edge/message" >/dev/null
done

# Poll workflow until it reaches SUCCEEDED and join step is SUCCEEDED.
for _ in $(seq 1 120); do
  wf_get="$(curl -fsS --noproxy "*" --max-time 10 \
    "${DAEMON_URL}/api/v1/edge/workflow?workflow_id=${WF_ID}&include_steps=1")"
  ok="$(python3 - <<PY
import json
obj = json.loads(r'''${wf_get}''')
wf = obj.get("workflow") or {}
steps = obj.get("steps") or []
st = wf.get("status")
join = None
for s in steps:
  if s.get("step_id") == "join_all":
    join = s.get("state")
print("1" if st == "SUCCEEDED" and join == "SUCCEEDED" else "0")
PY
)"
  if [[ "${ok}" == "1" ]]; then
    break
  fi
  sleep 0.1
done

wf_get="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/workflow?workflow_id=${WF_ID}&include_steps=1")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${wf_get}''')
if not obj.get("ok"):
  print("workflow get not ok", obj, file=sys.stderr)
  raise SystemExit(1)
wf = obj.get("workflow") or {}
if wf.get("status") != "SUCCEEDED":
  print("workflow did not reach SUCCEEDED", wf, file=sys.stderr)
  raise SystemExit(1)
steps = obj.get("steps") or []
by = {s.get("step_id"): s for s in steps if isinstance(s, dict)}
if (by.get("join_all") or {}).get("state") != "SUCCEEDED":
  print("join step not SUCCEEDED", by.get("join_all"), file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

# Cancel via message.
WF2_ID="$(python3 - <<'PY'
import uuid
print("wf_" + str(uuid.uuid4()))
PY
)"

wf2_submit_msg="$(python3 - <<PY
import json, uuid, time
deadline = int(time.time()*1000) + 120_000
workflow = {
  "workflow_id": "${WF2_ID}",
  "goal": "cancel via WORKFLOW_CANCEL message",
  "priority": 1,
  "steps": [
    {
      "step_id": "only",
      "kind": "invoke_tool",
      "depends_on": [],
      "target": {"node_id": "${NODE_ID}"},
      "payload": {"tool":"ui.led.ws2812.control","args":{"action":"solid"}},
      "deadline_utc_ms": deadline,
      "max_attempts": 3,
      "backoff_ms": 250
    }
  ]
}
env = {
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "WORKFLOW_SUBMIT",
  "from": "node:${NODE_ID}",
  "to": "platform",
  "body": workflow,
}
print(json.dumps(env))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${wf2_submit_msg}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

wf2_cancel_msg="$(python3 - <<PY
import json, uuid, time
env = {
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "WORKFLOW_CANCEL",
  "from": "node:${NODE_ID}",
  "to": "platform",
  "body": {"workflow_id": "${WF2_ID}"},
}
print(json.dumps(env))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${wf2_cancel_msg}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

for _ in $(seq 1 120); do
  wf_get="$(curl -fsS --noproxy "*" --max-time 10 \
    "${DAEMON_URL}/api/v1/edge/workflow?workflow_id=${WF2_ID}&include_steps=1")"
  ok="$(python3 - <<PY
import json
obj = json.loads(r'''${wf_get}''')
wf = obj.get("workflow") or {}
steps = obj.get("steps") or []
st = wf.get("status")
s = None
for step in steps:
  if step.get("step_id") == "only":
    s = step.get("state")
print("1" if st == "CANCELED" and s == "CANCELED" else "0")
PY
)"
  if [[ "${ok}" == "1" ]]; then
    echo "agentd_edge_workflow_submit_message_smoke OK"
    exit 0
  fi
  sleep 0.1
done

echo "workflow cancel did not reach CANCELED in time" >&2
exit 1
