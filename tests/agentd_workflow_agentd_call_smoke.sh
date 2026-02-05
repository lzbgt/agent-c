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

HOST="127.0.0.1"
PORT_REMOTE="$(agentd_smoke_pick_port)"
PORT_LOCAL="$(agentd_smoke_pick_port)"
NAME="agentd_workflow_agentd_call_smoke"

cleanup() {
  # Stop local (agentd_smoke_stop uses AGENTD_PID global).
  agentd_smoke_stop

  # Stop remote.
  if [[ -n "${REMOTE_PID:-}" ]]; then
    kill -TERM "${REMOTE_PID}" >/dev/null 2>&1 || true
    for _ in $(seq 1 30); do
      if ! kill -0 "${REMOTE_PID}" >/dev/null 2>&1; then
        break
      fi
      sleep 0.1
    done
    if kill -0 "${REMOTE_PID}" >/dev/null 2>&1; then
      kill -KILL "${REMOTE_PID}" >/dev/null 2>&1 || true
    fi
    wait "${REMOTE_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

# Start a remote agentd instance (the collaboration target).
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_REMOTE}" "${NAME}_remote" \
  --tools none
REMOTE_PID="${AGENTD_PID}"
REMOTE_URL="${DAEMON_URL}"
agentd_smoke_wait_health "${REMOTE_URL}"

# Start a local agentd instance (runs the workflow with kind:"agentd_call").
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_LOCAL}" "${NAME}_local" \
  --tools none \
  --workflow-enable-http-tasks
LOCAL_URL="${DAEMON_URL}"
agentd_smoke_wait_health "${LOCAL_URL}"

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
tasks = [
  {
    "task_id": "REMOTE",
    "kind": "agentd_call",
    "agentd_call": {
      "base_url": "${REMOTE_URL}",
      "op": "workflow_submit_and_wait",
      "timeout_ms": 20000,
      "poll_ms": 20,
      "max_bytes": 1048576,
      "include_tasks": True,
      "include_results": True,
      "workflow": {
        "tasks": [
          {"task_id": "W", "kind": "delay", "delay_ms": 10, "result": {"assistant_text": "remote ok"}}
        ]
      }
    }
  }
]
print(json.dumps({"tasks": tasks}))
PY
)" \
  "${LOCAL_URL}/api/v1/workflow/submit")"

workflow_id="$(python3 - <<PY
import json
obj = json.loads(r'''${submit_resp}''')
print(obj.get("workflow_id",""))
PY
)"
if [[ -z "${workflow_id}" ]]; then
  echo "failed to get workflow_id: ${submit_resp}" >&2
  exit 1
fi

final=""
for _ in $(seq 1 400); do
  final="$(curl -fsS --noproxy "*" --max-time 5 \
    "${LOCAL_URL}/api/v1/workflow?workflow_id=${workflow_id}&include_tasks=1&include_results=1")"
  if python3 - <<PY >/dev/null 2>&1
import json
obj = json.loads(r'''${final}''')
w = obj.get("workflow") or {}
st = w.get("status")
raise SystemExit(0 if st in ("done","error","cancelled") else 1)
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
if by_id.get("REMOTE", {}).get("status") != "done":
  print("agentd_call task not done", by_id.get("REMOTE"), file=sys.stderr)
  raise SystemExit(1)

res = obj.get("result") or {}
by = res.get("results_by_task") or {}
r = by.get("REMOTE") or {}
if not r.get("ok"):
  print("agentd_call result not ok", r, file=sys.stderr)
  raise SystemExit(1)
agentd = r.get("agentd") or {}
if not agentd.get("workflow_id"):
  print("missing remote workflow_id", agentd, file=sys.stderr)
  raise SystemExit(1)
final = agentd.get("final") or {}
wf2 = final.get("workflow") or {}
if wf2.get("status") != "done":
  print("expected remote workflow status done", wf2, file=sys.stderr)
  raise SystemExit(1)
r2 = (final.get("result") or {}).get("results_by_task") or {}
wres = r2.get("W") or {}
if wres.get("assistant_text") != "remote ok":
  print("expected remote task assistant_text", wres, file=sys.stderr)
  raise SystemExit(1)
PY

