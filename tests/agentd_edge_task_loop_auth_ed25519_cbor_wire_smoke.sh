#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
ENCODER_BIN="${2:-}"
ED_TOOL_BIN="${3:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "missing agentd binary path arg" >&2
  exit 2
fi
if [[ -z "${ENCODER_BIN}" || ! -x "${ENCODER_BIN}" ]]; then
  echo "missing/non-executable encoder binary path arg: ${ENCODER_BIN}" >&2
  exit 2
fi
if [[ -z "${ED_TOOL_BIN}" || ! -x "${ED_TOOL_BIN}" ]]; then
  echo "missing/non-executable agent_ed25519_tool binary path arg: ${ED_TOOL_BIN}" >&2
  exit 2
fi

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

PORT_DAEMON="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"

trap agentd_smoke_stop EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_task_loop_auth_ed25519_cbor_wire_smoke" \
  --tools none
agentd_smoke_wait_health "${DAEMON_URL}"

NODE_ID="node_auth_task_ed_cbor_1"
KID_NODE="${NODE_ID}"
SK_HEX="0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"
CAPS_SHA="sha256:$(python3 - <<'PY'
print("d"*64)
PY
)"

PK_B64="$("${ED_TOOL_BIN}" --sk-hex "${SK_HEX}" --print-pk-b64)"
if [[ -z "${PK_B64}" ]]; then
  echo "failed to compute pk base64" >&2
  exit 1
fi

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
  "edge_auth_ed25519_pubkeys": {"${KID_NODE}": "${PK_B64}"},
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

post_cbor_expect_401() {
  local status
  status="$(curl -sS --noproxy "*" --max-time 10 \
    -o /dev/null -w "%{http_code}" \
    -H "Content-Type: application/cbor" \
    --data-binary @- \
    "${DAEMON_URL}/api/v1/edge/message")"
  if [[ "${status}" != "401" ]]; then
    echo "expected 401, got ${status}" >&2
    exit 1
  fi
}

"${ENCODER_BIN}" \
  --type NODE_HELLO \
  --node-id "${NODE_ID}" \
  --msg-id "ed_auth_task_msg_1" \
  --caps-sha256 "${CAPS_SHA}" \
  --auth-alg ed25519-cbor \
  --auth-kid "${KID_NODE}" \
  --auth-seq 1 \
  --ed25519-sk-hex "${SK_HEX}" | post_cbor_expect_ok

"${ENCODER_BIN}" \
  --type NODE_CAPS_RSP \
  --node-id "${NODE_ID}" \
  --msg-id "ed_auth_task_msg_2" \
  --caps-sha256 "${CAPS_SHA}" \
  --manifest-minimal \
  --enforce-det \
  --auth-alg ed25519-cbor \
  --auth-kid "${KID_NODE}" \
  --auth-seq 2 \
  --ed25519-sk-hex "${SK_HEX}" | post_cbor_expect_ok

DEADLINE_UTC_MS="$(python3 - <<'PY'
import time
print(int(time.time() * 1000) + 60000)
PY
)"

TASK_ID="task_ed_auth_cbor_1"
STEP_ID="s1"
IDEM="idem_ed_auth_cbor_1"

assign_json="$(
  python3 - <<PY
import json
print(json.dumps({
  "node_id": "${NODE_ID}",
  "task_id": "${TASK_ID}",
  "step_id": "${STEP_ID}",
  "idempotency_key": "${IDEM}",
  "mode": "agent",
  "deadline_utc_ms": int("${DEADLINE_UTC_MS}"),
  "payload": {"prompt": "Send TASK_DONE."},
}))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  --data "${assign_json}" \
  "${DAEMON_URL}/api/v1/edge/task/assign" >/dev/null

"${ENCODER_BIN}" \
  --type TASK_ACK \
  --node-id "${NODE_ID}" \
  --msg-id "ed_auth_task_msg_3" \
  --task-id "${TASK_ID}" \
  --step-id "${STEP_ID}" \
  --idempotency-key "${IDEM}" \
  --accepted 1 \
  --auth-alg ed25519-cbor \
  --auth-kid "${KID_NODE}" \
  --auth-seq 3 \
  --ed25519-sk-hex "${SK_HEX}" | post_cbor_expect_ok

"${ENCODER_BIN}" \
  --type TASK_EVENT \
  --node-id "${NODE_ID}" \
  --msg-id "ed_auth_task_msg_4" \
  --task-id "${TASK_ID}" \
  --step-id "${STEP_ID}" \
  --idempotency-key "${IDEM}" \
  --state "running" \
  --progress 0.5 \
  --auth-alg ed25519-cbor \
  --auth-kid "${KID_NODE}" \
  --auth-seq 4 \
  --ed25519-sk-hex "${SK_HEX}" | post_cbor_expect_ok

"${ENCODER_BIN}" \
  --type TASK_EVENT \
  --node-id "${NODE_ID}" \
  --msg-id "ed_auth_task_msg_4_replay" \
  --task-id "${TASK_ID}" \
  --step-id "${STEP_ID}" \
  --idempotency-key "${IDEM}" \
  --state "running" \
  --progress 0.6 \
  --auth-alg ed25519-cbor \
  --auth-kid "${KID_NODE}" \
  --auth-seq 4 \
  --ed25519-sk-hex "${SK_HEX}" | post_cbor_expect_401

"${ENCODER_BIN}" \
  --type TASK_DONE \
  --node-id "${NODE_ID}" \
  --msg-id "ed_auth_task_msg_5" \
  --task-id "${TASK_ID}" \
  --step-id "${STEP_ID}" \
  --idempotency-key "${IDEM}" \
  --result-ok 1 \
  --result-text "ok-from-ed25519-auth-cbor" \
  --auth-alg ed25519-cbor \
  --auth-kid "${KID_NODE}" \
  --auth-seq 5 \
  --ed25519-sk-hex "${SK_HEX}" | post_cbor_expect_ok

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
PY

echo "agentd_edge_task_loop_auth_ed25519_cbor_wire_smoke OK"
