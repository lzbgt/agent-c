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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_rules_sensor_event_auth_hmac_cbor_wire_smoke" \
  --tools none
agentd_smoke_wait_health "${DAEMON_URL}"

NODE_ID="node_rule_auth_cbor_1"
KID_NODE="${NODE_ID}"
SECRET_NODE="test_secret_node_123"
CAPS_SHA="sha256:$(python3 - <<'PY'
print("d"*64)
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

# Signed bring-up with deterministic manifest containing an invoke tool schema.
SEQ=1

"${ENCODER_BIN}" \
  --type NODE_HELLO \
  --node-id "${NODE_ID}" \
  --msg-id "auth_rule_msg_1" \
  --caps-sha256 "${CAPS_SHA}" \
  --auth-alg hmac-sha256-cbor \
  --auth-kid "${KID_NODE}" \
  --auth-seq "${SEQ}" \
  --hmac-secret "${SECRET_NODE}" | post_cbor_expect_ok
SEQ=$((SEQ + 1))

"${ENCODER_BIN}" \
  --type NODE_CAPS_RSP \
  --node-id "${NODE_ID}" \
  --msg-id "auth_rule_msg_2" \
  --caps-sha256 "${CAPS_SHA}" \
  --manifest-minimal-ws2812 \
  --enforce-det \
  --auth-alg hmac-sha256-cbor \
  --auth-kid "${KID_NODE}" \
  --auth-seq "${SEQ}" \
  --hmac-secret "${SECRET_NODE}" | post_cbor_expect_ok
SEQ=$((SEQ + 1))

# Create an automation rule: SENSOR_EVENT -> TASK_ASSIGN (invoke tool).
rule_upsert="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "event_type": "dirt_detected",
  "enabled": True,
  "min_confidence": 0.8,
  "cooldown_ms": 0,
  "action": {
    "type": "task_assign",
    "mode": "invoke",
    "deadline_in_ms": 60000,
    "target": {"node_id": "${NODE_ID}"},
    "payload": {"tool":"ui.led.ws2812.control","args":{"action":"blink"}}
  }
}))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/rule/upsert")"

RULE_ID="$(python3 - <<PY
import json
obj = json.loads(r'''${rule_upsert}''')
if not obj.get("ok"):
  raise SystemExit(1)
print(obj.get("rule_id") or "")
PY
)"
if [[ -z "${RULE_ID}" ]]; then
  echo "missing rule_id in response: ${rule_upsert}" >&2
  exit 1
fi

# Emit SENSOR_EVENT over the signed CBOR wire profile and assert a TASK_ASSIGN appears in outbox.
"${ENCODER_BIN}" \
  --type SENSOR_EVENT \
  --node-id "${NODE_ID}" \
  --msg-id "auth_rule_msg_3" \
  --event-type "dirt_detected" \
  --confidence 0.9 \
  --data-text "dirt" \
  --auth-alg hmac-sha256-cbor \
  --auth-kid "${KID_NODE}" \
  --auth-seq "${SEQ}" \
  --hmac-secret "${SECRET_NODE}" | post_cbor_expect_ok
SEQ=$((SEQ + 1))

task_pair=""
for _ in $(seq 1 40); do
  outbox="$(curl -fsS --noproxy "*" --max-time 10 \
    "${DAEMON_URL}/api/v1/edge/outbox?node_id=${NODE_ID}&cursor=0&limit=200")"
  task_pair="$(python3 - <<PY
import json
obj = json.loads(r'''${outbox}''')
msgs = obj.get("messages") or []
for m in msgs:
  env = m.get("msg") or {}
  if env.get("type") != "TASK_ASSIGN":
    continue
  body = env.get("body") or {}
  payload = body.get("payload") or {}
  if payload.get("tool") != "ui.led.ws2812.control":
    continue
  print(body.get("task_id") or "")
  print(body.get("step_id") or "")
  print(body.get("idempotency_key") or "")
  raise SystemExit(0)
raise SystemExit(1)
PY
)" && break || true
  sleep 0.1
done
if [[ -z "${task_pair}" ]]; then
  echo "failed to observe TASK_ASSIGN in outbox" >&2
  echo "outbox: ${outbox:-}" >&2
  exit 1
fi

TASK_ID="$(echo "${task_pair}" | sed -n '1p')"
STEP_ID="$(echo "${task_pair}" | sed -n '2p')"
IDEM_KEY="$(echo "${task_pair}" | sed -n '3p')"
if [[ -z "${TASK_ID}" || -z "${STEP_ID}" || -z "${IDEM_KEY}" ]]; then
  echo "failed to extract task_id/step_id from outbox: ${outbox}" >&2
  exit 1
fi

# Complete the task over the same signed CBOR transport (proves end-to-end rule->task->done loop).
"${ENCODER_BIN}" \
  --type TASK_ACK \
  --node-id "${NODE_ID}" \
  --msg-id "auth_rule_msg_4" \
  --task-id "${TASK_ID}" \
  --step-id "${STEP_ID}" \
  --idempotency-key "${IDEM_KEY}" \
  --accepted 1 \
  --auth-alg hmac-sha256-cbor \
  --auth-kid "${KID_NODE}" \
  --auth-seq "${SEQ}" \
  --hmac-secret "${SECRET_NODE}" | post_cbor_expect_ok
SEQ=$((SEQ + 1))

"${ENCODER_BIN}" \
  --type TASK_DONE \
  --node-id "${NODE_ID}" \
  --msg-id "auth_rule_msg_5" \
  --task-id "${TASK_ID}" \
  --step-id "${STEP_ID}" \
  --idempotency-key "${IDEM_KEY}" \
  --result-ok 1 \
  --result-text "rule=${RULE_ID}" \
  --auth-alg hmac-sha256-cbor \
  --auth-kid "${KID_NODE}" \
  --auth-seq "${SEQ}" \
  --hmac-secret "${SECRET_NODE}" | post_cbor_expect_ok

task_get="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/task?task_id=${TASK_ID}&step_id=${STEP_ID}")"
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
PY

echo "agentd_edge_rules_sensor_event_auth_hmac_cbor_wire_smoke OK"

