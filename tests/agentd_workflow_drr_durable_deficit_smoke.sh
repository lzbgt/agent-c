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
NAME="agentd_workflow_drr_durable_deficit_smoke"

DB_PATH="${LOG_DIR}/${NAME}_${PORT_DAEMON}.sqlite"
STATE_DIR="${LOG_DIR}/${NAME}_${PORT_DAEMON}.state"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

# Phase 1: initialize DB schema then stop.
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}_init" \
  --db-path "${DB_PATH}" \
  --state-dir "${STATE_DIR}" \
  --tools none \
  --workflow-concurrency 1 \
  --workflow-poll-ms 200 \
  --workflow-fair-queue-policy drr

agentd_smoke_wait_health "${DAEMON_URL}"
agentd_smoke_stop

# Inject a persisted deficit so session A is briefly "in debt" after restart. This forces session B
# to be selected first even if its workflow is newer / lower priority.
python3 - <<PY
import sqlite3, time
db_path = r"""${DB_PATH}"""
now = int(time.time() * 1000)
conn = sqlite3.connect(db_path)
cur = conn.cursor()
cur.execute("""
INSERT OR REPLACE INTO workflow_fairq_sessions(session_id, deficit, weight, updated_unix_ms)
VALUES(?,?,?,?);
""", ("sess_A", -1, 1, now))
cur.execute("""
INSERT OR REPLACE INTO workflow_fairq_sessions(session_id, deficit, weight, updated_unix_ms)
VALUES(?,?,?,?);
""", ("sess_B", 0, 1, now))
conn.commit()
conn.close()
PY

# Phase 2: restart daemon; scheduler should load the deficit and skip A until it replenishes.
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}" \
  --db-path "${DB_PATH}" \
  --state-dir "${STATE_DIR}" \
  --tools none \
  --workflow-concurrency 1 \
  --workflow-poll-ms 500 \
  --workflow-max-inflight-per-workflow 1 \
  --workflow-fair-queue-policy drr

agentd_smoke_wait_health "${DAEMON_URL}"

submit_one() {
  local session_id="${1}"
  local tag="${2}"
  curl -fsS --noproxy "*" --max-time 20 \
    -H "Content-Type: application/json" \
    -d "$(python3 - <<PY
import json
print(json.dumps({
  "allow_sessions": True,
  "session_id": "${session_id}",
  "session_weight": 1,
  "tasks": [
    {"task_id": "T", "kind": "delay", "delay_ms": 30, "result": {"assistant_text": "${tag}"}}
  ]
}))
PY
)" \
    "${DAEMON_URL}/api/v1/workflow/submit"
}

resp_b="$(submit_one "sess_B" "B")"
wid_b="$(python3 - <<PY
import json
print(json.loads(r'''${resp_b}''').get("workflow_id",""))
PY
)"
resp_a="$(submit_one "sess_A" "A")"
wid_a="$(python3 - <<PY
import json
print(json.loads(r'''${resp_a}''').get("workflow_id",""))
PY
)"

if [[ -z "${wid_a}" || -z "${wid_b}" ]]; then
  echo "failed to get workflow_ids: A=${resp_a} B=${resp_b}" >&2
  exit 1
fi

python3 - <<PY
import json, subprocess, sys, time

daemon_url = r"""${DAEMON_URL}"""
wid_a = r"""${wid_a}"""
wid_b = r"""${wid_b}"""

def get_wf(wid: str):
  raw = subprocess.check_output([
    "curl", "-fsS", "--noproxy", "*", "--max-time", "10",
    f"{daemon_url}/api/v1/workflow?workflow_id={wid}&include_tasks=1&include_results=1"
  ], text=True)
  obj = json.loads(raw)
  if not obj.get("ok"):
    raise RuntimeError(f"workflow get failed: {wid} {obj}")
  wf = obj.get("workflow") or {}
  tasks = obj.get("tasks") or []
  return wf, tasks

done = {}
deadline = time.time() + 10.0
while time.time() < deadline:
  for wid in (wid_a, wid_b):
    if wid in done:
      continue
    wf, _ = get_wf(wid)
    st = wf.get("status")
    if st in ("done", "error", "cancelled"):
      done[wid] = int(wf.get("updated_unix_ms") or 0)
  if len(done) == 2:
    break
  time.sleep(0.02)

if len(done) != 2:
  print("timed out waiting for both workflows done", done, file=sys.stderr)
  raise SystemExit(1)

# With sess_A injected as "in debt", sess_B should be admitted first.
if done[wid_b] <= 0 or done[wid_a] <= 0:
  print("missing updated_unix_ms", done, file=sys.stderr)
  raise SystemExit(1)
if not (done[wid_b] <= done[wid_a]):
  print("expected sess_B to finish before sess_A due to persisted deficit", file=sys.stderr)
  print("wid_a", wid_a, "wid_b", wid_b, file=sys.stderr)
  print("done", done, file=sys.stderr)
  raise SystemExit(1)

print("ok: durable DRR deficit influenced scheduling order:", done)
PY

echo "${NAME} OK"
