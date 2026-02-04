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

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_workflow_inflight_session_cap_smoke" \
  --tools none \
  --workflow-concurrency 2 \
  --workflow-max-inflight-per-workflow 2 \
  --workflow-max-inflight-per-session 1

agentd_smoke_wait_health "${DAEMON_URL}"

SESSION_ID="sess_shared"

submit_one() {
  local task_id="${1}"
  curl -fsS --noproxy "*" --max-time 20 \
    -H "Content-Type: application/json" \
    -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${SESSION_ID}",
  "tasks": [
    {"task_id": "${task_id}", "kind": "delay", "delay_ms": 750, "result": {"assistant_text": "${task_id}"}}
  ]
}))
PY
)" \
    "${DAEMON_URL}/api/v1/workflow/submit"
}

wf1_submit="$(submit_one "A")"
wf2_submit="$(submit_one "B")"

wf1_id="$(python3 - <<PY
import json
print(json.loads(r'''${wf1_submit}''').get("workflow_id",""))
PY
)"
wf2_id="$(python3 - <<PY
import json
print(json.loads(r'''${wf2_submit}''').get("workflow_id",""))
PY
)"

if [[ -z "${wf1_id}" || -z "${wf2_id}" ]]; then
  echo "failed to get workflow ids: wf1=${wf1_submit} wf2=${wf2_submit}" >&2
  exit 1
fi

wait_done() {
  local wid="${1}"
  local final=""
  for _ in $(seq 1 260); do
    final="$(curl -fsS --noproxy "*" --max-time 5 \
      "${DAEMON_URL}/api/v1/workflow?workflow_id=${wid}&include_tasks=1")"
    if python3 - <<PY >/dev/null 2>&1
import json, sys
obj = json.loads(r'''${final}''')
w = obj.get("workflow") or {}
raise SystemExit(0 if w.get("status") in ("done","error","cancelled") else 1)
PY
    then
      echo "${final}"
      return 0
    fi
    sleep 0.05
  done
  echo "${final}"
  return 1
}

wf1_final="$(wait_done "${wf1_id}")"
wf2_final="$(wait_done "${wf2_id}")"

python3 - <<PY
import json, sys
wf1 = json.loads(r'''${wf1_final}''')
wf2 = json.loads(r'''${wf2_final}''')
for label, obj in (("wf1", wf1), ("wf2", wf2)):
  if not obj.get("ok"):
    print(label, "get failed", obj, file=sys.stderr)
    raise SystemExit(1)
  w = obj.get("workflow") or {}
  if w.get("status") != "done":
    print(label, "expected done", w, file=sys.stderr)
    raise SystemExit(1)

def extract_times(obj):
  tasks = obj.get("tasks") or []
  if len(tasks) != 1:
    return None
  t = tasks[0]
  return (t.get("task_id"), int(t.get("started_unix_ms") or 0), int(t.get("finished_unix_ms") or 0), t.get("status"))

t1 = extract_times(wf1)
t2 = extract_times(wf2)
if not t1 or not t2:
  print("missing task rows", wf1.get("tasks"), wf2.get("tasks"), file=sys.stderr)
  raise SystemExit(1)
id1, s1, f1, st1 = t1
id2, s2, f2, st2 = t2
if st1 != "done" or st2 != "done":
  print("tasks not done", t1, t2, file=sys.stderr)
  raise SystemExit(1)
if not (s1 > 0 and f1 > 0 and s2 > 0 and f2 > 0):
  print("missing timestamps", t1, t2, file=sys.stderr)
  raise SystemExit(1)

# Per-session cap correctness: with max_inflight_per_session=1, these two workflows must not overlap.
skew = 25
if s1 <= s2:
  if s2 < f1 - skew:
    print("overlap detected (wf1 then wf2)", {"s1":s1,"f1":f1,"s2":s2,"f2":f2}, file=sys.stderr)
    raise SystemExit(1)
else:
  if s1 < f2 - skew:
    print("overlap detected (wf2 then wf1)", {"s1":s1,"f1":f1,"s2":s2,"f2":f2}, file=sys.stderr)
    raise SystemExit(1)
PY

