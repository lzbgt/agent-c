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
NAME="agentd_workflow_stats_smoke"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}" \
  --tools none \
  --workflow-concurrency 2 \
  --workflow-max-inflight-per-workflow 2

agentd_smoke_wait_health "${DAEMON_URL}"

stats0="$(curl -fsS --noproxy "*" --max-time 10 "${DAEMON_URL}/api/v1/workflow/stats")"
python3 - <<PY
import json, sys
obj = json.loads(r'''${stats0}''')
if not obj.get("ok"):
  print("stats not ok", obj, file=sys.stderr)
  raise SystemExit(1)
wb = obj.get("workflows_by_status") or {}
tb = obj.get("tasks_by_status") or {}
def total(m):
  return sum(int(v) for v in m.values() if isinstance(v, (int,float)))
if total(wb) != 0 or total(tb) != 0:
  print("expected empty stats at start", obj, file=sys.stderr)
  raise SystemExit(1)
PY

submit_one() {
  local task_id="${1}"
  curl -fsS --noproxy "*" --max-time 20 \
    -H "Content-Type: application/json" \
    -d "$(python3 - <<PY
import json
print(json.dumps({
  "tasks": [
    {"task_id": "${task_id}", "kind": "delay", "delay_ms": 600, "result": {"assistant_text": "${task_id}"}}
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
  echo "missing workflow ids wf1=${wf1_submit} wf2=${wf2_submit}" >&2
  exit 1
fi

stats1="$(curl -fsS --noproxy "*" --max-time 10 "${DAEMON_URL}/api/v1/workflow/stats")"
python3 - <<PY
import json, sys
obj = json.loads(r'''${stats1}''')
if not obj.get("ok"):
  print("stats not ok", obj, file=sys.stderr)
  raise SystemExit(1)
wb = obj.get("workflows_by_status") or {}
tb = obj.get("tasks_by_status") or {}
def total(m):
  return sum(int(v) for v in m.values() if isinstance(v, (int,float)))
if total(wb) != 2:
  print("expected exactly 2 workflows total", wb, file=sys.stderr)
  raise SystemExit(1)
if total(tb) != 2:
  print("expected exactly 2 tasks total", tb, file=sys.stderr)
  raise SystemExit(1)
PY

wait_done() {
  local wid="${1}"
  for _ in $(seq 1 260); do
    snap="$(curl -fsS --noproxy "*" --max-time 5 \
      "${DAEMON_URL}/api/v1/workflow?workflow_id=${wid}")"
    if python3 - <<PY >/dev/null 2>&1
import json, sys
obj = json.loads(r'''${snap}''')
w = obj.get("workflow") or {}
raise SystemExit(0 if w.get("status") in ("done","error","cancelled") else 1)
PY
    then
      return 0
    fi
    sleep 0.05
  done
  return 1
}

wait_done "${wf1_id}"
wait_done "${wf2_id}"

stats2="$(curl -fsS --noproxy "*" --max-time 10 "${DAEMON_URL}/api/v1/workflow/stats")"
python3 - <<PY
import json, sys
obj = json.loads(r'''${stats2}''')
if not obj.get("ok"):
  print("stats not ok", obj, file=sys.stderr)
  raise SystemExit(1)
wb = obj.get("workflows_by_status") or {}
tb = obj.get("tasks_by_status") or {}
def total(m):
  return sum(int(v) for v in m.values() if isinstance(v, (int,float)))
if total(wb) != 2 or total(tb) != 2:
  print("expected totals preserved", obj, file=sys.stderr)
  raise SystemExit(1)
if int(wb.get("done") or 0) != 2:
  print("expected workflows done=2", wb, file=sys.stderr)
  raise SystemExit(1)
if int(tb.get("done") or 0) != 2:
  print("expected tasks done=2", tb, file=sys.stderr)
  raise SystemExit(1)
PY

