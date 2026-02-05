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

DB_PATH="${LOG_DIR}/agentd_edge_task_attest_smoke_${PORT_DAEMON}.sqlite"
STATE_DIR="${LOG_DIR}/agentd_edge_task_attest_smoke_${PORT_DAEMON}.state"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_task_attest_smoke" \
  --db-path "${DB_PATH}" \
  --state-dir "${STATE_DIR}" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

TASK_ID="t_attest_$(date +%s)_$RANDOM"
STEP_ID="s1"
NODE_ID="node_attest_1"
IDEM="k_attest_1"

assign_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(TASK_ID="${TASK_ID}" STEP_ID="${STEP_ID}" NODE_ID="${NODE_ID}" IDEM="${IDEM}" python3 - <<'PY'
import json, os, time
now = int(time.time() * 1000)
payload = {
  "node_id": os.environ.get("NODE_ID") or "",
  "task_id": os.environ.get("TASK_ID") or "",
  "step_id": os.environ.get("STEP_ID") or "",
  "idempotency_key": os.environ.get("IDEM") or "",
  "mode": "agent",
  "deadline_utc_ms": now + 60000,
  "attempt": 0,
  "payload": {"prompt": "ping"},
}
print(json.dumps(payload))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/task/assign")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${assign_resp}''')
if not obj.get("ok"):
  print("assign failed", obj, file=sys.stderr)
  raise SystemExit(1)
PY

# Send TASK_DONE with a node-provided attest blob. Platform should:
# - persist result_json
# - compute and persist result_sha256 (sha256 of stored result_json bytes)
# - persist attest_json (and surface it on GET /edge/task)
node_attest="sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
done_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(TASK_ID="${TASK_ID}" STEP_ID="${STEP_ID}" NODE_ID="${NODE_ID}" IDEM="${IDEM}" NODE_ATTEST="${node_attest}" python3 - <<'PY'
import json, os, time, uuid
now = int(time.time() * 1000)
env = {
  "msg_id": "m_" + uuid.uuid4().hex,
  "ts_utc_ms": now,
  "type": "TASK_DONE",
  "from": "node:" + (os.environ.get("NODE_ID") or "node"),
  "to": "platform",
  "body": {
    "task_id": os.environ.get("TASK_ID") or "",
    "step_id": os.environ.get("STEP_ID") or "",
    "idempotency_key": os.environ.get("IDEM") or "",
    "result": {
      "ok": True,
      "assistant_text": "ok",
      "attest": {
        "result_sha256": os.environ.get("NODE_ATTEST") or "",
        "note": "node-provided demo token",
      },
    },
  },
}
print(json.dumps(env))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/message")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${done_resp}''')
if not obj.get("ok"):
  print("TASK_DONE failed", obj, file=sys.stderr)
  raise SystemExit(1)
PY

task_get="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/task?task_id=${TASK_ID}&step_id=${STEP_ID}")"

python3 - <<PY
import json, re, sys
obj = json.loads(r'''${task_get}''')
if not obj.get("ok"):
  print("edge task get failed", obj, file=sys.stderr)
  raise SystemExit(1)
t = obj.get("task") or {}
if t.get("task_id") != "${TASK_ID}":
  print("task_id mismatch", t.get("task_id"), file=sys.stderr)
  raise SystemExit(1)
if t.get("state") != "SUCCEEDED":
  print("expected SUCCEEDED got", t.get("state"), file=sys.stderr)
  raise SystemExit(1)
rsha = t.get("result_sha256") or ""
if not re.fullmatch(r"sha256:[0-9a-f]{64}", rsha):
  print("missing/invalid result_sha256", rsha, file=sys.stderr)
  raise SystemExit(1)
att = t.get("attest") or {}
if att.get("result_sha256") != "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef":
  print("attest.result_sha256 mismatch", att, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_edge_task_attest_smoke OK"

