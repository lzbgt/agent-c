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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_trace_edge_interop_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

NODE_ID="node_trace_1"
CAPS_SHA="sha256:abababababababababababababababababababababababababababababababab"
TOOL_NAME="ui.led.ws2812.control"
TRACE_ID="edge_trace_$(date +%s)_$RANDOM"

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

TASK_ID="t_trace"
STEP_ID="s"
IDEMPOTENCY_KEY="k_trace"

assign_json="$(python3 - <<PY
import json
print(json.dumps({
  "node_id": "${NODE_ID}",
  "task_id": "${TASK_ID}",
  "step_id": "${STEP_ID}",
  "idempotency_key": "${IDEMPOTENCY_KEY}",
  "mode": "invoke",
  "deadline_utc_ms": int("${deadline_utc_ms}"),
  "trace": {"trace_id": "${TRACE_ID}"},
  "payload": {"tool": "${TOOL_NAME}", "args": {"action": "on"}},
}))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${assign_json}" \
  "${DAEMON_URL}/api/v1/edge/task/assign" >/dev/null

outbox="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/outbox?node_id=${NODE_ID}&cursor=0&limit=50")"

if ! python3 - <<PY
import json, sys
obj = json.loads(r'''${outbox}''')
msgs = obj.get("messages") or []
found = False
for m in msgs:
  env = (m.get("msg") or {})
  if env.get("type") != "TASK_ASSIGN":
    continue
  trace = env.get("trace") or {}
  if trace.get("trace_id") != "${TRACE_ID}":
    print("TASK_ASSIGN trace mismatch:", trace, file=sys.stderr)
    raise SystemExit(1)
  found = True
  break
if not found:
  print("expected TASK_ASSIGN in outbox", file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY
then
  echo "outbox: ${outbox}" >&2
  exit 1
fi

done_json="$(python3 - <<PY
import json, uuid, time
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "TASK_DONE",
  "from": f"node:{'${NODE_ID}'}",
  "to": "platform",
  "trace": {"trace_id": "${TRACE_ID}"},
  "body": {
    "task_id": "${TASK_ID}",
    "step_id": "${STEP_ID}",
    "idempotency_key": "${IDEMPOTENCY_KEY}",
    "result": {"ok": True, "data": {"applied": True}}
  }
}))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${done_json}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

trace_q="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/trace?trace_id=$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote('${TRACE_ID}'))
PY
)&limit=200&max_bytes=1048576")"

if ! python3 - <<PY
import json, sys
q = json.loads(r'''${trace_q}''')
if not q.get("ok"):
  print("trace query failed:", q, file=sys.stderr)
  raise SystemExit(1)
recs = q.get("records") or []
if not isinstance(recs, list) or len(recs) == 0:
  print("expected trace records", file=sys.stderr)
  raise SystemExit(1)
found = False
for r in recs:
  if not isinstance(r, dict):
    continue
  if r.get("source") != "edge_task_event":
    continue
  ev = r.get("event") or {}
  trace = ev.get("trace") or {}
  if trace.get("trace_id") == "${TRACE_ID}":
    found = True
    break
if not found:
  print("expected at least one edge_task_event record for trace_id", file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY
then
  echo "trace_q: ${trace_q}" >&2
  exit 1
fi

echo "agentd_trace_edge_interop_smoke OK"

