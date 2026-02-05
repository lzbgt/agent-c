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
AUTH_TOKEN="agentd_workflow_edge_parallel_quorum_hashes_default_pointers_smoke_token"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_workflow_edge_parallel_quorum_hashes_default_pointers_smoke" \
  --auth-token "${AUTH_TOKEN}" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

NODE_A="node_test_edge_qdef_A"
NODE_B="node_test_edge_qdef_B"
CAPS_SHA_A="sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
CAPS_SHA_B="sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

send_hello_and_caps() {
  local node_id="$1"
  local caps_sha="$2"

  local hello_json
  hello_json="$(python3 - <<PY
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
    "caps_sha256": "${caps_sha}",
  }
}))
PY
)"

  curl -fsS --noproxy "*" --max-time 10 \
    -H "Authorization: Bearer ${AUTH_TOKEN}" \
    -H "Content-Type: application/json" \
    -d "${hello_json}" \
    "${DAEMON_URL}/api/v1/edge/message" >/dev/null

  local caps_rsp_json
  caps_rsp_json="$(python3 - <<PY
import json, uuid, time
manifest = {
  "spec_version": "um-acds/0.1",
  "manifest_version": "0.0.1",
  "caps_sha256": "${caps_sha}",
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
    "caps_sha256": "${caps_sha}",
    "manifest": manifest
  }
}))
PY
)"

  curl -fsS --noproxy "*" --max-time 10 \
    -H "Authorization: Bearer ${AUTH_TOKEN}" \
    -H "Content-Type: application/json" \
    -d "${caps_rsp_json}" \
    "${DAEMON_URL}/api/v1/edge/message" >/dev/null
}

send_hello_and_caps "${NODE_A}" "${CAPS_SHA_A}"
send_hello_and_caps "${NODE_B}" "${CAPS_SHA_B}"

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Authorization: Bearer ${AUTH_TOKEN}" \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
tasks = [
  {"task_id":"P","kind":"edge_parallel","max_attempts":200,
   "edge_parallel":{
     "count":2,
     "edge":{
       "match_any":{"requires_tools":["ui.led.ws2812.control"]},
       "tool":"ui.led.ws2812.control",
       "args":{"action":"solid"},
       "timeout_ms":5000
     },
     "aggregate": {
       "mode": "quorum_hashes",
       "quorum": 2,
       "require_distinct_nodes": True
     }
   }}
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

wait_for_assign() {
  local node_id="$1"
  local step_id="$2"

  local outbox=""
  for _ in $(seq 1 200); do
    outbox="$(curl -fsS --noproxy "*" --max-time 10 \
      -H "Authorization: Bearer ${AUTH_TOKEN}" \
      "${DAEMON_URL}/api/v1/edge/outbox?node_id=${node_id}&cursor=0&limit=50")" || true
    if python3 - <<PY >/dev/null 2>&1
import json
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

  echo "timed out waiting for TASK_ASSIGN node_id=${node_id} step_id=${step_id}" >&2
  return 1
}

STEP_A="P:${NODE_A}"
STEP_B="P:${NODE_B}"

wait_for_assign "${NODE_A}" "${STEP_A}"
wait_for_assign "${NODE_B}" "${STEP_B}"

send_ack_and_done() {
  local node_id="$1"
  local step_id="$2"

  curl -fsS --noproxy "*" --max-time 10 \
    -H "Authorization: Bearer ${AUTH_TOKEN}" \
    -H "Content-Type: application/json" \
    -d "$(python3 - <<PY
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
)" \
    "${DAEMON_URL}/api/v1/edge/message" >/dev/null

  # Return identical results across nodes so edge_result_sha256 reaches quorum.
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Authorization: Bearer ${AUTH_TOKEN}" \
    -H "Content-Type: application/json" \
    -d "$(python3 - <<PY
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
    "result":{
      "ok":True,
      "data":{"applied":{"action":"solid"}},
      "attest":{"result_sha256":"sha256:" + ("0"*64), "kid":"test-kid", "sig":"test-sig"}
    }
  },
}))
PY
)" \
    "${DAEMON_URL}/api/v1/edge/message" >/dev/null
}

send_ack_and_done "${NODE_A}" "${STEP_A}"
send_ack_and_done "${NODE_B}" "${STEP_B}"

final=""
for _ in $(seq 1 240); do
  final="$(curl -fsS --noproxy "*" --max-time 5 \
    -H "Authorization: Bearer ${AUTH_TOKEN}" \
    "${DAEMON_URL}/api/v1/workflow?workflow_id=${workflow_id}&include_tasks=1&include_results=1")"
  if python3 - <<PY >/dev/null 2>&1
import json
obj = json.loads(r'''${final}''')
w = obj.get("workflow") or {}
st = w.get("status")
raise SystemExit(0 if st in ("done","error","cancelled") else 1)
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

res = obj.get("result") or {}
by = res.get("results_by_task") or {}
p = by.get("P") or {}
if p.get("kind") != "aggregate" or p.get("mode") != "quorum_hashes" or p.get("ok") is not True:
  print("expected P to be aggregate quorum_hashes ok=true", p, file=sys.stderr)
  raise SystemExit(1)

if p.get("pointers") != ["/edge_result_sha256"]:
  print("unexpected pointers default", p.get("pointers"), file=sys.stderr)
  raise SystemExit(1)
if p.get("node_pointer") != "/edge/node_id":
  print("unexpected node_pointer default", p.get("node_pointer"), file=sys.stderr)
  raise SystemExit(1)
if p.get("require_distinct_nodes") is not True:
  print("expected require_distinct_nodes true", p, file=sys.stderr)
  raise SystemExit(1)

checks = p.get("checks") or []
by_ptr = {c.get("ptr"): c for c in checks if isinstance(c, dict)}
c = by_ptr.get("/edge_result_sha256") or {}
if c.get("ok") is not True or int(c.get("chosen_count", 0)) != 2:
  print("unexpected /edge_result_sha256 quorum check", c, file=sys.stderr)
  raise SystemExit(1)
chosen = c.get("chosen") or ""
if not (isinstance(chosen, str) and chosen.startswith("sha256:") and len(chosen) == 71):
  print("unexpected chosen token", chosen, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_workflow_edge_parallel_quorum_hashes_default_pointers_smoke OK"

