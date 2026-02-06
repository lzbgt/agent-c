#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
ENCODER_BIN="${2:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "missing agentd binary path arg" >&2
  exit 2
fi
if [[ -z "${ENCODER_BIN}" || ! -x "${ENCODER_BIN}" ]]; then
  echo "missing/non-executable encoder binary path arg: ${ENCODER_BIN}" >&2
  exit 2
fi

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

PORT_DAEMON="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"

trap agentd_smoke_stop EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_invoke_task_loop_auth_hmac_cbor_wire_smoke" \
  --tools none
agentd_smoke_wait_health "${DAEMON_URL}"

NODE_ID="node_auth_invoke_cbor_1"
KID_NODE="${NODE_ID}"
SECRET_NODE="test_secret_node_123"
CAPS_SHA="sha256:$(python3 - <<'PY'
print("e"*64)
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "edge_auth_required": True,
  "edge_auth_require_ts": True,
  "edge_auth_max_skew_ms": 60000,
  "edge_auth_require_seq": True,
  "edge_auth_kid_policy": "match_node",
  "edge_auth_hmac_keys": {"${KID_NODE}": "${SECRET_NODE}"},
}))
PY
)" \
  "${DAEMON_URL}/api/v1/config/update" >/dev/null

post_cbor_expect_ok() {
  local resp
  resp="$(curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/cbor" \
    --data-binary @- \
    "${DAEMON_URL}/api/v1/edge/message")"
  python3 - <<PY
import json, sys
obj = json.loads(r'''${resp}''')
if not obj.get("ok"):
  print("expected ok true", obj, file=sys.stderr)
  raise SystemExit(1)
PY
}

# Signed bring-up with a deterministic manifest that contains an invoke tool schema.
"${ENCODER_BIN}" \
  --type NODE_HELLO \
  --node-id "${NODE_ID}" \
  --msg-id "auth_invoke_msg_1" \
  --caps-sha256 "${CAPS_SHA}" \
  --auth-alg hmac-sha256-cbor \
  --auth-kid "${KID_NODE}" \
  --auth-seq 1 \
  --hmac-secret "${SECRET_NODE}" | post_cbor_expect_ok

"${ENCODER_BIN}" \
  --type NODE_CAPS_RSP \
  --node-id "${NODE_ID}" \
  --msg-id "auth_invoke_msg_2" \
  --caps-sha256 "${CAPS_SHA}" \
  --manifest-minimal-ws2812 \
  --enforce-det \
  --auth-alg hmac-sha256-cbor \
  --auth-kid "${KID_NODE}" \
  --auth-seq 2 \
  --hmac-secret "${SECRET_NODE}" | post_cbor_expect_ok

# Negative: invalid args should be rejected by platform-side schema validation (additionalProperties:false + required action).
BAD_DEADLINE_UTC_MS="$(python3 - <<'PY'
import time
print(int(time.time() * 1000) + 60000)
PY
)"

status_bad="$(
  curl -sS --noproxy "*" --max-time 10 \
    -o /dev/null -w "%{http_code}" \
    -H "Content-Type: application/json" \
    -d "$(python3 - <<PY
import json
print(json.dumps({
  "node_id": "${NODE_ID}",
  "task_id": "task_bad_args_1",
  "step_id": "s1",
  "idempotency_key": "idem_bad_args_1",
  "mode": "invoke",
  "deadline_utc_ms": int("${BAD_DEADLINE_UTC_MS}"),
  "payload": {
    "tool": "ui.led.ws2812.control",
    "args": {"extra": 1}
  }
}))
PY
)" \
    "${DAEMON_URL}/api/v1/edge/task/assign"
)"
if [[ "${status_bad}" != "400" ]]; then
  echo "expected 400 for invalid invoke args, got ${status_bad}" >&2
  exit 1
fi

DEADLINE_UTC_MS="$(python3 - <<'PY'
import time
print(int(time.time() * 1000) + 60000)
PY
)"

TASK_ID="task_invoke_auth_cbor_1"
STEP_ID="s1"
IDEM="idem_invoke_auth_cbor_1"

assign_resp="$(
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/json" \
    -d "$(python3 - <<PY
import json
print(json.dumps({
  "node_id": "${NODE_ID}",
  "task_id": "${TASK_ID}",
  "step_id": "${STEP_ID}",
  "idempotency_key": "${IDEM}",
  "mode": "invoke",
  "deadline_utc_ms": int("${DEADLINE_UTC_MS}"),
  "payload": {
    "tool": "ui.led.ws2812.control",
    "args": {"action": "blink"}
  }
}))
PY
)" \
    "${DAEMON_URL}/api/v1/edge/task/assign"
)"
python3 - <<PY
import json, sys
obj = json.loads(r'''${assign_resp}''')
if not obj.get("ok"):
  print("expected ok true (assign)", obj, file=sys.stderr)
  raise SystemExit(1)
PY

"${ENCODER_BIN}" \
  --type TASK_ACK \
  --node-id "${NODE_ID}" \
  --msg-id "auth_invoke_msg_3" \
  --task-id "${TASK_ID}" \
  --step-id "${STEP_ID}" \
  --idempotency-key "${IDEM}" \
  --accepted 1 \
  --auth-alg hmac-sha256-cbor \
  --auth-kid "${KID_NODE}" \
  --auth-seq 3 \
  --hmac-secret "${SECRET_NODE}" | post_cbor_expect_ok

"${ENCODER_BIN}" \
  --type TASK_EVENT \
  --node-id "${NODE_ID}" \
  --msg-id "auth_invoke_msg_4" \
  --task-id "${TASK_ID}" \
  --step-id "${STEP_ID}" \
  --idempotency-key "${IDEM}" \
  --state "running" \
  --progress 0.5 \
  --auth-alg hmac-sha256-cbor \
  --auth-kid "${KID_NODE}" \
  --auth-seq 4 \
  --hmac-secret "${SECRET_NODE}" | post_cbor_expect_ok

"${ENCODER_BIN}" \
  --type TASK_DONE \
  --node-id "${NODE_ID}" \
  --msg-id "auth_invoke_msg_5" \
  --task-id "${TASK_ID}" \
  --step-id "${STEP_ID}" \
  --idempotency-key "${IDEM}" \
  --result-ok 1 \
  --result-text "ok-from-invoke-auth-cbor" \
  --auth-alg hmac-sha256-cbor \
  --auth-kid "${KID_NODE}" \
  --auth-seq 5 \
  --hmac-secret "${SECRET_NODE}" | post_cbor_expect_ok

task_json="$(curl -fsS --noproxy "*" --max-time 10 "${DAEMON_URL}/api/v1/edge/task?task_id=${TASK_ID}&step_id=${STEP_ID}")"
python3 - <<PY
import json, sys
obj = json.loads(r'''${task_json}''')
if not obj.get("ok"):
  print("task get failed", obj, file=sys.stderr)
  raise SystemExit(1)
t = obj.get("task") or {}
if t.get("state") != "SUCCEEDED":
  print("expected SUCCEEDED", t, file=sys.stderr)
  raise SystemExit(1)
res = t.get("result") or {}
data = res.get("data") or {}
if data.get("text") != "ok-from-invoke-auth-cbor":
  print("expected result.data.text", res, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_edge_invoke_task_loop_auth_hmac_cbor_wire_smoke OK"

# Also prove fail-closed `result_schema` enforcement over the same CBOR+auth transport:
# send TASK_DONE without `result.data` (our encoder omits it when --result-text is not set).
TASK_ID2="task_invoke_auth_cbor_bad_result_1"
STEP_ID2="s1"
IDEM2="idem_invoke_auth_cbor_bad_result_1"

assign_resp2="$(
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/json" \
    -d "$(python3 - <<PY
import json, time
print(json.dumps({
  "node_id": "${NODE_ID}",
  "task_id": "${TASK_ID2}",
  "step_id": "${STEP_ID2}",
  "idempotency_key": "${IDEM2}",
  "mode": "invoke",
  "deadline_utc_ms": int(time.time() * 1000) + 60000,
  "payload": {
    "tool": "ui.led.ws2812.control",
    "args": {"action": "blink"}
  }
}))
PY
)" \
    "${DAEMON_URL}/api/v1/edge/task/assign"
)"
python3 - <<PY
import json, sys
obj = json.loads(r'''${assign_resp2}''')
if not obj.get("ok"):
  print("expected ok true (assign2)", obj, file=sys.stderr)
  raise SystemExit(1)
PY

"${ENCODER_BIN}" \
  --type TASK_ACK \
  --node-id "${NODE_ID}" \
  --msg-id "auth_invoke_msg_6" \
  --task-id "${TASK_ID2}" \
  --step-id "${STEP_ID2}" \
  --idempotency-key "${IDEM2}" \
  --accepted 1 \
  --auth-alg hmac-sha256-cbor \
  --auth-kid "${KID_NODE}" \
  --auth-seq 6 \
  --hmac-secret "${SECRET_NODE}" | post_cbor_expect_ok

"${ENCODER_BIN}" \
  --type TASK_DONE \
  --node-id "${NODE_ID}" \
  --msg-id "auth_invoke_msg_7" \
  --task-id "${TASK_ID2}" \
  --step-id "${STEP_ID2}" \
  --idempotency-key "${IDEM2}" \
  --result-ok 1 \
  --auth-alg hmac-sha256-cbor \
  --auth-kid "${KID_NODE}" \
  --auth-seq 7 \
  --hmac-secret "${SECRET_NODE}" | post_cbor_expect_ok

task2_json="$(curl -fsS --noproxy "*" --max-time 10 "${DAEMON_URL}/api/v1/edge/task?task_id=${TASK_ID2}&step_id=${STEP_ID2}")"
python3 - <<PY
import json, sys
obj = json.loads(r'''${task2_json}''')
if not obj.get("ok"):
  print("task2 get failed", obj, file=sys.stderr)
  raise SystemExit(1)
t = obj.get("task") or {}
if t.get("state") != "FAILED":
  print("expected FAILED for schema mismatch", t, file=sys.stderr)
  raise SystemExit(1)
err = t.get("error") or ""
if "result_schema" not in err:
  print("expected result_schema in error", t, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_edge_invoke_task_loop_auth_hmac_cbor_wire_smoke (bad result_schema) OK"
