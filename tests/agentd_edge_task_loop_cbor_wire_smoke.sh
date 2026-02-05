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

ENCODER_BIN="${2:-}"
if [[ -z "${ENCODER_BIN}" || ! -x "${ENCODER_BIN}" ]]; then
  echo "missing/non-executable encoder binary path arg: ${ENCODER_BIN}" >&2
  exit 2
fi

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

HOST="127.0.0.1"
PORT="$(agentd_smoke_pick_port)"

trap agentd_smoke_stop EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "agentd_edge_task_loop_cbor_wire_smoke" \
  --tools none
agentd_smoke_wait_health "${DAEMON_URL}"

NODE_ID="node_cbor_task_1"
CAPS="sha256:$(python3 - <<'PY'
print("b"*64)
PY
)"

post_cbor() {
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/cbor" \
    --data-binary @- \
    "${DAEMON_URL}/api/v1/edge/message"
}

MSG_ID_HELLO="cbor_task_msg_1"
resp="$(
  "${ENCODER_BIN}" \
    --type NODE_HELLO \
    --node-id "${NODE_ID}" \
    --msg-id "${MSG_ID_HELLO}" \
    --model "esp32" \
    --fw-git-sha "deadbeef" \
    --caps-sha256 "${CAPS}" | post_cbor
)"
python3 - <<PY
import json, sys
obj = json.loads(r'''${resp}''')
if not obj.get("ok"):
  print("expected ok true (NODE_HELLO)", obj, file=sys.stderr)
  raise SystemExit(1)
PY

# Complete the capability handshake over CBOR wire (not strictly required for mode=agent tasks,
# but keeps the test aligned with the full interop bring-up flow).
MSG_ID_CAPS="cbor_task_msg_2"
resp_caps="$(
  "${ENCODER_BIN}" \
    --type NODE_CAPS_RSP \
    --node-id "${NODE_ID}" \
    --msg-id "${MSG_ID_CAPS}" \
    --caps-sha256 "${CAPS}" \
    --manifest-minimal \
    --enforce-det | post_cbor
)"
python3 - <<PY
import json, sys
obj = json.loads(r'''${resp_caps}''')
if not obj.get("ok"):
  print("expected ok true (NODE_CAPS_RSP)", obj, file=sys.stderr)
  raise SystemExit(1)
PY

# Enqueue a TASK_ASSIGN (platform helper). Use mode=agent so no tool manifest requirements apply.
DEADLINE_UTC_MS="$(python3 - <<'PY'
import time
print(int(time.time() * 1000) + 60000)
PY
)"

TASK_ID="task_cbor_wire_1"
STEP_ID="s1"
IDEM="idem_cbor_wire_1"

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
  "payload": {
    "prompt": "Return TASK_DONE with a short result string."
  }
}))
PY
)"

assign_resp="$(
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/json" \
    --data "${assign_json}" \
    "${DAEMON_URL}/api/v1/edge/task/assign"
)"
python3 - <<PY
import json, sys
obj = json.loads(r'''${assign_resp}''')
if not obj.get("ok"):
  print("expected ok true (task assign)", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("node_id") != "${NODE_ID}":
  print("node_id mismatch", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("task_id") != "${TASK_ID}" or obj.get("step_id") != "${STEP_ID}":
  print("task/step mismatch", obj, file=sys.stderr)
  raise SystemExit(1)
PY

# Node sends lifecycle messages as application/cbor and the platform updates durable task state.
MSG_ID_ACK="cbor_task_msg_3"
ack_resp="$(
  "${ENCODER_BIN}" \
    --type TASK_ACK \
    --node-id "${NODE_ID}" \
    --msg-id "${MSG_ID_ACK}" \
    --task-id "${TASK_ID}" \
    --step-id "${STEP_ID}" \
    --idempotency-key "${IDEM}" \
    --accepted 1 | post_cbor
)"
python3 - <<PY
import json, sys
obj = json.loads(r'''${ack_resp}''')
if not obj.get("ok"):
  print("expected ok true (TASK_ACK)", obj, file=sys.stderr)
  raise SystemExit(1)
PY

MSG_ID_EVENT="cbor_task_msg_4"
event_resp="$(
  "${ENCODER_BIN}" \
    --type TASK_EVENT \
    --node-id "${NODE_ID}" \
    --msg-id "${MSG_ID_EVENT}" \
    --task-id "${TASK_ID}" \
    --step-id "${STEP_ID}" \
    --idempotency-key "${IDEM}" \
    --state "running" \
    --progress 0.5 | post_cbor
)"
python3 - <<PY
import json, sys
obj = json.loads(r'''${event_resp}''')
if not obj.get("ok"):
  print("expected ok true (TASK_EVENT)", obj, file=sys.stderr)
  raise SystemExit(1)
PY

MSG_ID_DONE="cbor_task_msg_5"
done_resp="$(
  "${ENCODER_BIN}" \
    --type TASK_DONE \
    --node-id "${NODE_ID}" \
    --msg-id "${MSG_ID_DONE}" \
    --task-id "${TASK_ID}" \
    --step-id "${STEP_ID}" \
    --idempotency-key "${IDEM}" \
    --result-ok 1 \
    --result-text "ok-from-cbor" | post_cbor
)"
python3 - <<PY
import json, sys
obj = json.loads(r'''${done_resp}''')
if not obj.get("ok"):
  print("expected ok true (TASK_DONE)", obj, file=sys.stderr)
  raise SystemExit(1)
PY

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
if res.get("ok") is not True:
  print("expected result.ok true", res, file=sys.stderr)
  raise SystemExit(1)
data = res.get("data") or {}
if data.get("text") != "ok-from-cbor":
  print("expected result.data.text", res, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_edge_task_loop_cbor_wire_smoke OK"

