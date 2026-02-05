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
NAME="agentd_edge_message_dedupe_replay_task_done_smoke"

DB_PATH="${LOG_DIR}/${NAME}_${PORT_DAEMON}.sqlite"
STATE_DIR="${LOG_DIR}/${NAME}_${PORT_DAEMON}.state"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

# Phase 1: create schema, then stop (simulates a prior run that crashed after persisting inbox rows).
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}_phase1" \
  --db-path "${DB_PATH}" \
  --state-dir "${STATE_DIR}" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"
agentd_smoke_stop

TASK_ID="t_dedupe_done_1"
STEP_ID="s1"
NODE_ID="node_fixture_1"
IDEM="idem_1"

MSG_ID="dedupe_task_done_msg_1"

ENV_JSON="$(python3 - <<PY
import json
env = {
  "msg_id": "${MSG_ID}",
  "ts_utc_ms": 1700000000000,
  "type": "TASK_DONE",
  "from": "node:${NODE_ID}",
  "to": "platform",
  "body": {
    "task_id": "${TASK_ID}",
    "step_id": "${STEP_ID}",
    "idempotency_key": "${IDEM}",
    "result": {"ok": True, "data": {"answer": "ok"}}
  }
}
print(json.dumps(env))
PY
)"

# Simulate crash window: inbox row exists, but task state updates never happened.
python3 - <<PY
import sqlite3, time
db = r'''${DB_PATH}'''
env_json = r'''${ENV_JSON}'''
now = int(time.time() * 1000)
con = sqlite3.connect(db)
cur = con.cursor()

# Seed the edge task row so TASK_DONE has something to update.
cur.execute(
  "INSERT OR REPLACE INTO edge_tasks(task_id, step_id, node_id, idempotency_key, mode, deadline_utc_ms, payload_json, state, created_utc_ms, updated_utc_ms) "
  "VALUES(?,?,?,?,?,?,?,?,?,?)",
  ("${TASK_ID}", "${STEP_ID}", "${NODE_ID}", "${IDEM}", "invoke", now + 60000, "{}", "RUNNING", now, now),
)

cur.execute(
  "INSERT OR REPLACE INTO edge_inbox_messages(msg_id, ts_utc_ms, type, from_id, to_id, envelope_json, processed, processed_utc_ms) "
  "VALUES(?,?,?,?,?,?,0,NULL)",
  ("${MSG_ID}", now, "TASK_DONE", "node:${NODE_ID}", "platform", env_json),
)
con.commit()
con.close()
PY

# Phase 2: restart agentd; a transport retry re-sends the same msg_id.
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}_phase2" \
  --db-path "${DB_PATH}" \
  --state-dir "${STATE_DIR}" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${ENV_JSON}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

# The task should be updated to SUCCEEDED and keep the result.
task_get="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/task?task_id=${TASK_ID}&step_id=${STEP_ID}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${task_get}''')
if not obj.get("ok"):
  print("task get failed", obj, file=sys.stderr)
  raise SystemExit(1)
t = obj.get("task") or {}
if t.get("state") != "SUCCEEDED":
  print("expected SUCCEEDED", t, file=sys.stderr)
  raise SystemExit(1)
res = t.get("result") or {}
if (res.get("data") or {}).get("answer") != "ok":
  print("missing result data", res, file=sys.stderr)
  raise SystemExit(1)
PY
