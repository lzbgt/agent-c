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
NAME="agentd_edge_message_dedupe_replay_handoff_smoke"

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

MSG_ID="dedupe_replay_msg_1"
WID="wf:dedupe_replay_1"

ENV_JSON="$(python3 - <<PY
import json
env = {
  "msg_id": "${MSG_ID}",
  "ts_utc_ms": 1700000000000,
  "type": "DURABLE_WORKFLOW_SUBMIT",
  "from": "node:fixture_node_1",
  "to": "platform",
  "body": {
    "workflow": {
      "workflow_id": "${WID}",
      "tasks": [
        {"task_id":"A","kind":"delay","delay_ms":0,"result":{"assistant_text":"OK"}}
      ]
    }
  }
}
print(json.dumps(env))
PY
)"

# Simulate crash window: inbox row exists, but side effects (workflow creation) never happened.
python3 - <<PY
import sqlite3
db = r'''${DB_PATH}'''
env_json = r'''${ENV_JSON}'''
con = sqlite3.connect(db)
cur = con.cursor()
cur.execute(
  "INSERT OR REPLACE INTO edge_inbox_messages(msg_id, ts_utc_ms, type, from_id, to_id, envelope_json) VALUES(?,?,?,?,?,?)",
  ("${MSG_ID}", 1700000000000, "DURABLE_WORKFLOW_SUBMIT", "node:fixture_node_1", "platform", env_json),
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

# The durable workflow should still be created and finish DONE, despite msg_id dedupe.
final=""
for _ in $(seq 1 160); do
  final="$(curl -fsS --noproxy "*" --max-time 10 \
    "${DAEMON_URL}/api/v1/workflow?workflow_id=${WID}&include_tasks=1&include_results=1")" || true
  if python3 - <<PY >/dev/null 2>&1
import json, sys
obj = json.loads(r'''${final}''') if r'''${final}''' else {}
w = obj.get("workflow") or {}
raise SystemExit(0 if w.get("status") in ("done","error","cancelled") else 1)
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
tasks = obj.get("tasks") or []
by_id = {t.get("task_id"): t for t in tasks if isinstance(t, dict)}
if by_id.get("A", {}).get("status") != "done":
  print("expected task A done", by_id.get("A"), file=sys.stderr)
  raise SystemExit(1)
PY

