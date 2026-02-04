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

DB_PATH="${LOG_DIR}/agentd_workflow_deadline_smoke_${PORT_DAEMON}.sqlite"
STATE_DIR="${LOG_DIR}/agentd_workflow_deadline_smoke_${PORT_DAEMON}.state"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_workflow_deadline_smoke" \
  --db-path "${DB_PATH}" \
  --state-dir "${STATE_DIR}" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<'PY'
import json

tasks = [
  {"task_id": "A", "kind": "delay", "delay_ms": 0, "result": {"assistant_text": "A"}},
  {"task_id": "B", "kind": "delay", "delay_ms": 0, "result": {"assistant_text": "B"}},
]

payload = {
  # Use a past deadline for deterministic behavior.
  "deadline_unix_ms": 1,
  "tasks": tasks,
}
print(json.dumps(payload))
PY
)" \
  "${DAEMON_URL}/api/v1/workflow/submit")"

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
for _ in $(seq 1 200); do
  final="$(curl -fsS --noproxy "*" --max-time 5 \
    "${DAEMON_URL}/api/v1/workflow?workflow_id=${workflow_id}&include_tasks=1&include_results=1")"
  if python3 - <<PY >/dev/null 2>&1
import json, sys
obj = json.loads(r'''${final}''')
w = obj.get("workflow") or {}
st = w.get("status")
if st in ("done","error","cancelled"):
  raise SystemExit(0)
raise SystemExit(1)
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
if w.get("status") != "cancelled":
  print("expected workflow status cancelled", w, file=sys.stderr)
  raise SystemExit(1)
if "deadline" not in ((w.get("error") or "").lower()):
  print("expected workflow error to mention deadline", w, file=sys.stderr)
  raise SystemExit(1)
tasks = obj.get("tasks") or []
by_id = {t.get("task_id"): t for t in tasks if isinstance(t, dict)}
for tid in ("A","B"):
  t = by_id.get(tid) or {}
  if t.get("status") != "cancelled":
    print("expected task cancelled", tid, t, file=sys.stderr)
    raise SystemExit(1)
  if "deadline" not in ((t.get("error") or "").lower()):
    print("expected task error to mention deadline", tid, t, file=sys.stderr)
    raise SystemExit(1)
PY

echo "agentd_workflow_deadline_smoke OK"

