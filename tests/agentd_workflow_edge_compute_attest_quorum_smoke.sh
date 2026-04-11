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
AUTH_TOKEN="agentd_workflow_edge_compute_attest_quorum_smoke_token"
NAME="agentd_workflow_edge_compute_attest_quorum_smoke"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}" \
  --auth-token "${AUTH_TOKEN}" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

NODE_A="node_compute_attest_A"
NODE_B="node_compute_attest_B"
CAPS_SHA="sha256:7777777777777777777777777777777777777777777777777777777777777777"
RESULT_HASH="sha256:8888888888888888888888888888888888888888888888888888888888888888"
TRACE_HASH="sha256:9999999999999999999999999999999999999999999999999999999999999999"
STATE_HASH="sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
PROGRAM_HASH="sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
JOB_HASH="sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"

send_edge_msg() {
  local json="${1:?}"
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Authorization: Bearer ${AUTH_TOKEN}" \
    -H "Content-Type: application/json" \
    -d "${json}" \
    "${DAEMON_URL}/api/v1/edge/message" >/dev/null
}

node_hello() {
  local node_id="${1:?}"
  python3 - <<PY
import json, time, uuid
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time() * 1000),
  "type": "NODE_HELLO",
  "from": "node:${node_id}",
  "to": "platform",
  "body": {
    "node_id": "${node_id}",
    "model": "esp32sim_compute_attest",
    "fw_git_sha": "deadbeef",
    "caps_sha256": "${CAPS_SHA}",
  },
}))
PY
}

node_caps_rsp() {
  local node_id="${1:?}"
  python3 - <<PY
import json, time, uuid
manifest = {
  "spec_version": "um-acds/0.1",
  "manifest_version": "0.0.1",
  "caps_sha256": "${CAPS_SHA}",
  "node": {"node_id": "${node_id}"},
  "runtime": {"agent_core": {"version": "0.0.0"}},
  "hardware": {"presence": {"avm.capsule": "present"}},
  "tools": [
    {
      "name": "avm.capsule.run",
      "kind": "compute",
      "description": "Run deterministic AVM capsule",
      "parameters_schema": {
        "type": "object",
        "additionalProperties": False,
        "properties": {"program": {"type": "string"}},
        "required": ["program"]
      },
      "timeout_ms": 1000,
      "idempotent": True,
      "side_effect_level": "none",
      "hazards": [],
      "tags": ["avm", "capsule"]
    }
  ],
  "tags": ["compute", "lab"]
}
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time() * 1000),
  "type": "NODE_CAPS_RSP",
  "from": "node:${node_id}",
  "to": "platform",
  "body": {"node_id": "${node_id}", "caps_sha256": "${CAPS_SHA}", "manifest": manifest},
}))
PY
}

send_edge_msg "$(node_hello "${NODE_A}")"
send_edge_msg "$(node_caps_rsp "${NODE_A}")"
send_edge_msg "$(node_hello "${NODE_B}")"
send_edge_msg "$(node_caps_rsp "${NODE_B}")"

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Authorization: Bearer ${AUTH_TOKEN}" \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
tasks = [
  {"task_id":"E1","kind":"edge_invoke","max_attempts":200,
   "edge":{"node_id":"${NODE_A}","tool":"avm.capsule.run","args":{"program":"capsule-demo"},"timeout_ms":5000}},
  {"task_id":"E2","kind":"edge_invoke","max_attempts":200,
   "edge":{"node_id":"${NODE_B}","tool":"avm.capsule.run","args":{"program":"capsule-demo"},"timeout_ms":5000}},
  {"task_id":"J","kind":"aggregate","depends_on":["E1","E2"],
   "aggregate":{
     "mode":"quorum_hashes",
     "task_ids":["E1","E2"],
     "quorum":2,
     "require_distinct_nodes":True,
     "node_pointer":"/edge/node_id",
     "pointers":[
       "/edge_attest/compute/hashes/result_hash",
       "/edge_attest/compute/hashes/trace_hash"
     ]
   }}
]
print(json.dumps({"tasks": tasks}))
PY
)" \
  "${DAEMON_URL}/api/v1/workflow/submit")"

workflow_id="$(python3 - <<PY
import json
print((json.loads(r'''${submit_resp}''')).get("workflow_id", ""))
PY
)"
if [[ -z "${workflow_id}" ]]; then
  echo "failed to get workflow_id: ${submit_resp}" >&2
  exit 1
fi

wait_outbox_assign() {
  local node_id="${1:?}"
  local step_id="${2:?}"
  for _ in $(seq 1 120); do
    local outbox
    outbox="$(curl -fsS --noproxy "*" --max-time 10 \
      -H "Authorization: Bearer ${AUTH_TOKEN}" \
      "${DAEMON_URL}/api/v1/edge/outbox?node_id=${node_id}&cursor=0&limit=50")" || true
    if python3 - <<PY >/dev/null 2>&1
import json
obj = json.loads(r'''${outbox}''') if r'''${outbox}''' else {}
for row in obj.get("messages") or []:
  env = row.get("msg") or {}
  if env.get("type") != "TASK_ASSIGN":
    continue
  body = env.get("body") or {}
  if body.get("task_id") != "${workflow_id}" or body.get("step_id") != "${step_id}":
    continue
  if body.get("idempotency_key") != "${workflow_id}:${step_id}":
    raise SystemExit(1)
  payload = body.get("payload") or {}
  if payload.get("tool") != "avm.capsule.run":
    raise SystemExit(1)
  args = payload.get("args") or {}
  raise SystemExit(0 if args.get("program") == "capsule-demo" else 1)
raise SystemExit(1)
PY
    then
      return 0
    fi
    sleep 0.05
  done
  echo "timed out waiting for TASK_ASSIGN for ${node_id}/${step_id}" >&2
  return 1
}

wait_outbox_assign "${NODE_A}" "E1"
wait_outbox_assign "${NODE_B}" "E2"

task_ack() {
  local node_id="${1:?}"
  local step_id="${2:?}"
  python3 - <<PY
import json, time, uuid
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time() * 1000),
  "type": "TASK_ACK",
  "from": "node:${node_id}",
  "to": "platform",
  "body": {
    "task_id": "${workflow_id}",
    "step_id": "${step_id}",
    "idempotency_key": "${workflow_id}:${step_id}",
    "accepted": True,
  },
}))
PY
}

task_done() {
  local node_id="${1:?}"
  local step_id="${2:?}"
  python3 - <<PY
import hashlib, json, time, uuid
result = {"ok": True, "data": {"capsule": "edge", "run": 1}}
canon = json.dumps(result, sort_keys=True, separators=(",", ":")).encode("utf-8")
result_sha = "sha256:" + hashlib.sha256(canon).hexdigest()
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time() * 1000),
  "type": "TASK_DONE",
  "from": "node:${node_id}",
  "to": "platform",
  "body": {
    "task_id": "${workflow_id}",
    "step_id": "${step_id}",
    "idempotency_key": "${workflow_id}:${step_id}",
    "result": {
      **result,
      "attest": {
        "result_sha256": result_sha,
        "hash_alg": "agent_json_c14n_v1",
        "compute": {
          "schema": "um_eais_compute_attest_v1",
          "engine": "avm",
          "capsule": {
            "program_hash_sha256": "${PROGRAM_HASH}",
            "job_hash_sha256": "${JOB_HASH}",
          },
          "hashes": {
            "result_hash": "${RESULT_HASH}",
            "trace_hash": "${TRACE_HASH}",
            "state_hash": "${STATE_HASH}",
          },
        },
      },
    },
  },
}))
PY
}

send_edge_msg "$(task_ack "${NODE_A}" "E1")"
send_edge_msg "$(task_done "${NODE_A}" "E1")"
send_edge_msg "$(task_ack "${NODE_B}" "E2")"
send_edge_msg "$(task_done "${NODE_B}" "E2")"

final=""
for _ in $(seq 1 240); do
  final="$(curl -fsS --noproxy "*" --max-time 5 \
    -H "Authorization: Bearer ${AUTH_TOKEN}" \
    "${DAEMON_URL}/api/v1/workflow?workflow_id=${workflow_id}&include_tasks=1&include_results=1")"
  if python3 - <<PY >/dev/null 2>&1
import json
obj = json.loads(r'''${final}''')
status = (obj.get("workflow") or {}).get("status")
raise SystemExit(0 if status in ("done", "error", "cancelled") else 1)
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

by_task = (obj.get("result") or {}).get("results_by_task") or {}
j = by_task.get("J") or {}
if j.get("kind") != "aggregate" or j.get("mode") != "quorum_hashes" or j.get("ok") is not True:
  print("expected J quorum_hashes ok=true", j, file=sys.stderr)
  raise SystemExit(1)
if j.get("require_distinct_nodes") is not True or j.get("node_pointer") != "/edge/node_id":
  print("unexpected distinct-node metadata", j, file=sys.stderr)
  raise SystemExit(1)

checks = {c.get("ptr"): c for c in (j.get("checks") or []) if isinstance(c, dict)}
for ptr, expected in {
  "/edge_attest/compute/hashes/result_hash": "${RESULT_HASH}",
  "/edge_attest/compute/hashes/trace_hash": "${TRACE_HASH}",
}.items():
  c = checks.get(ptr) or {}
  if c.get("ok") is not True or c.get("chosen") != expected or c.get("chosen_count") != 2:
    print("unexpected aggregate check", ptr, c, file=sys.stderr)
    raise SystemExit(1)
  values_by_node = c.get("values_by_node") or {}
  if values_by_node.get("${NODE_A}") != expected or values_by_node.get("${NODE_B}") != expected:
    print("expected both nodes voting same compute hash", ptr, values_by_node, file=sys.stderr)
    raise SystemExit(1)

att_by_task = j.get("attestations_by_task_id") or {}
for tid in ("E1", "E2"):
  att = att_by_task.get(tid) or {}
  compute = att.get("compute") or {}
  hashes = compute.get("hashes") or {}
  capsule = compute.get("capsule") or {}
  if compute.get("schema") != "um_eais_compute_attest_v1" or compute.get("engine") != "avm":
    print("missing compute attestation", tid, att, file=sys.stderr)
    raise SystemExit(1)
  if hashes.get("result_hash") != "${RESULT_HASH}" or hashes.get("trace_hash") != "${TRACE_HASH}" or hashes.get("state_hash") != "${STATE_HASH}":
    print("unexpected compute hashes", tid, hashes, file=sys.stderr)
    raise SystemExit(1)
  if capsule.get("program_hash_sha256") != "${PROGRAM_HASH}" or capsule.get("job_hash_sha256") != "${JOB_HASH}":
    print("unexpected capsule hashes", tid, capsule, file=sys.stderr)
    raise SystemExit(1)

print("agentd_workflow_edge_compute_attest_quorum_smoke OK")
PY
