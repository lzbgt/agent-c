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
NAME="agentd_workflow_scan_fairness_smoke"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}" \
  --tools none \
  --workflow-concurrency 1 \
  --workflow-poll-ms 10 \
  --workflow-max-inflight-per-workflow 1

agentd_smoke_wait_health "${DAEMON_URL}"

submit_one() {
  local session_id="${1}"
  local tag="${2}"
  local delay_ms="${3}"
  curl -fsS --noproxy "*" --max-time 20 \
    -H "Content-Type: application/json" \
    -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${session_id}",
  "tasks": [
    {"task_id": "T", "kind": "delay", "delay_ms": int(${delay_ms}), "result": {"assistant_text": "${tag}"}}
  ]
}))
PY
)" \
    "${DAEMON_URL}/api/v1/workflow/submit"
}

# Regression: with DB-side LIMIT (default max_scan_workflows=64), an older workflow can starve when
# >64 newer workflows exist, because it never appears in the top-N scan window.
#
# This test submits:
# - 1 workflow in session S2 (older)
# - 520 workflows in session S1 (newer; exceeds the DB scan clamp of 512)
#
# Expected: S2 must complete early (before 20 total workflows reach done).
S2_SUBMIT="$(submit_one "sess_s2" "s2" 75)"
S2_ID="$(python3 - <<PY
import json
print(json.loads(r'''${S2_SUBMIT}''').get("workflow_id",""))
PY
)"
if [[ -z "${S2_ID}" ]]; then
  echo "failed to get S2 workflow_id: ${S2_SUBMIT}" >&2
  exit 1
fi

for i in $(seq 1 520); do
  submit_one "sess_s1" "s1_${i}" 15 >/dev/null
done

stats=""
done_cnt=0
for _ in $(seq 1 400); do
  stats="$(curl -fsS --noproxy "*" --max-time 5 "${DAEMON_URL}/api/v1/workflow/stats")"
  done_cnt="$(python3 - <<PY
import json
obj = json.loads(r'''${stats}''')
wb = obj.get("workflows_by_status") or {}
print(int(wb.get("done") or 0))
PY
)"
  if [[ "${done_cnt}" -ge 20 ]]; then
    break
  fi
  sleep 0.05
done
if [[ "${done_cnt}" -lt 20 ]]; then
  echo "timed out waiting for done_cnt>=20 (got ${done_cnt}); stats=${stats}" >&2
  exit 1
fi

snap_s2="$(curl -fsS --noproxy "*" --max-time 5 "${DAEMON_URL}/api/v1/workflow?workflow_id=${S2_ID}&include_tasks=1")"
python3 - <<PY
import json, sys
stats = json.loads(r'''${stats}''')
snap = json.loads(r'''${snap_s2}''')
if not snap.get("ok"):
  print("S2 snapshot not ok", snap, file=sys.stderr)
  raise SystemExit(1)
w = snap.get("workflow") or {}
st = w.get("status")
if st != "done":
  print("expected S2 done early; got", st, file=sys.stderr)
  print("done_cnt_at_check", ${done_cnt}, file=sys.stderr)
  print("stats", stats, file=sys.stderr)
  print("s2", snap, file=sys.stderr)
  raise SystemExit(1)
tasks = snap.get("tasks") or []
if len(tasks) != 1 or tasks[0].get("status") != "done":
  print("expected S2 single task done", tasks, file=sys.stderr)
  raise SystemExit(1)
PY
