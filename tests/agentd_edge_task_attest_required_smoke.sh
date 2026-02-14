#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
ED_TOOL="${2:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "missing agentd binary path arg" >&2
  exit 2
fi
if [[ -z "${ED_TOOL}" ]]; then
  echo "missing agent_ed25519_tool binary path arg" >&2
  exit 2
fi

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

PORT_DAEMON="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"

DB_PATH="${LOG_DIR}/agentd_edge_task_attest_required_smoke_${PORT_DAEMON}.sqlite"
STATE_DIR="${LOG_DIR}/agentd_edge_task_attest_required_smoke_${PORT_DAEMON}.state"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_task_attest_required_smoke" \
  --db-path "${DB_PATH}" \
  --state-dir "${STATE_DIR}" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

NODE_ID="node_attest_required_1"
CAPS_SHA="sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
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
        "properties": {"action": {"type": "string"}},
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

# Deterministic seed for ed25519 (RFC8032 vector #1 seed).
SK_HEX="9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"
KID="${NODE_ID}"

PK_B64="$("${ED_TOOL}" --sk-hex "${SK_HEX}" --print-pk-b64 | tr -d '\r\n')"
if [[ -z "${PK_B64}" ]]; then
  echo "failed to compute pk b64" >&2
  exit 1
fi

# Enforce attestation policy (invoke-mode only).
curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "edge_attest_required": True,
  "edge_attest_require_sig": True,
  "edge_auth_ed25519_pubkeys": {"${KID}": "${PK_B64}"},
}))
PY
)" \
  "${DAEMON_URL}/api/v1/config/update" >/dev/null

TASK_ID_BAD="t_attest_req_bad_$(date +%s)_$RANDOM"
STEP_ID="s1"
IDEM_BAD="k_attest_req_bad"

assign_bad="$(python3 - <<PY
import json, time
now = int(time.time() * 1000)
print(json.dumps({
  "node_id": "${NODE_ID}",
  "task_id": "${TASK_ID_BAD}",
  "step_id": "${STEP_ID}",
  "idempotency_key": "${IDEM_BAD}",
  "mode": "invoke",
  "deadline_utc_ms": now + 60000,
  "payload": {"tool": "${TOOL_NAME}", "args": {"action": "on"}},
}))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${assign_bad}" \
  "${DAEMON_URL}/api/v1/edge/task/assign" >/dev/null

done_bad="$(python3 - <<PY
import json, time, uuid
now = int(time.time() * 1000)
env = {
  "msg_id": "m_" + uuid.uuid4().hex,
  "ts_utc_ms": now,
  "type": "TASK_DONE",
  "from": "node:${NODE_ID}",
  "to": "platform",
  "body": {
    "task_id": "${TASK_ID_BAD}",
    "step_id": "${STEP_ID}",
    "idempotency_key": "${IDEM_BAD}",
    "result": {"ok": True, "data": {"applied": {"action": "on"}}},
  },
}
print(json.dumps(env))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${done_bad}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

task_bad="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/task?task_id=${TASK_ID_BAD}&step_id=${STEP_ID}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${task_bad}''')
if not obj.get("ok"):
  print("edge task get failed", obj, file=sys.stderr)
  raise SystemExit(1)
t = obj.get("task") or {}
if t.get("state") != "FAILED":
  print("expected FAILED for missing attest got", t.get("state"), file=sys.stderr)
  raise SystemExit(1)
if (t.get("error") or "") != "attest_required":
  print("expected error attest_required got", t.get("error"), file=sys.stderr)
  raise SystemExit(1)
PY

TASK_ID_OK="t_attest_req_ok_$(date +%s)_$RANDOM"
IDEM_OK="k_attest_req_ok"

assign_ok="$(python3 - <<PY
import json, time
now = int(time.time() * 1000)
print(json.dumps({
  "node_id": "${NODE_ID}",
  "task_id": "${TASK_ID_OK}",
  "step_id": "${STEP_ID}",
  "idempotency_key": "${IDEM_OK}",
  "mode": "invoke",
  "deadline_utc_ms": now + 60000,
  "payload": {"tool": "${TOOL_NAME}", "args": {"action": "on"}},
}))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${assign_ok}" \
  "${DAEMON_URL}/api/v1/edge/task/assign" >/dev/null

done_ok="$(ED25519_TOOL="${ED_TOOL}" python3 - <<PY
import hashlib, json, os, subprocess, time, uuid
tool = os.environ["ED25519_TOOL"]
sk_hex = "${SK_HEX}"
now = int(time.time() * 1000)
result = {"ok": True, "data": {"applied": {"action": "on"}}}
canon = json.dumps(result, sort_keys=True, separators=(",", ":")).encode("utf-8")
result_sha = "sha256:" + hashlib.sha256(canon).hexdigest()
sig_input = (
  "UM_EAIS_RESULT_ATTEST_v0_1\\n"
  + "${TASK_ID_OK}" + "\\n"
  + "${STEP_ID}" + "\\n"
  + "${IDEM_OK}" + "\\n"
  + result_sha + "\\n"
  + str(now) + "\\n"
).encode("utf-8")
sig_b64 = subprocess.check_output([tool, "--sk-hex", sk_hex], input=sig_input).decode("utf-8").strip()
env = {
  "msg_id": "m_" + uuid.uuid4().hex,
  "ts_utc_ms": now,
  "type": "TASK_DONE",
  "from": "node:${NODE_ID}",
  "to": "platform",
  "body": {
    "task_id": "${TASK_ID_OK}",
    "step_id": "${STEP_ID}",
    "idempotency_key": "${IDEM_OK}",
    "result": {
      **result,
      "attest": {
        "result_sha256": result_sha,
        "kid": "${KID}",
        "alg": "ed25519",
        "sig": sig_b64,
        "ts_utc_ms": now,
      },
    },
  },
}
print(json.dumps(env))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${done_ok}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

task_ok="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/task?task_id=${TASK_ID_OK}&step_id=${STEP_ID}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${task_ok}''')
if not obj.get("ok"):
  print("edge task get failed", obj, file=sys.stderr)
  raise SystemExit(1)
t = obj.get("task") or {}
if t.get("state") != "SUCCEEDED":
  print("expected SUCCEEDED got", t.get("state"), file=sys.stderr)
  raise SystemExit(1)
if t.get("error"):
  print("unexpected error field", t.get("error"), file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_edge_task_attest_required_smoke OK"
