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

DB_PATH="${LOG_DIR}/agentd_workflow_infer_deps_smoke_${PORT_DAEMON}.sqlite"
STATE_DIR="${LOG_DIR}/agentd_workflow_infer_deps_smoke_${PORT_DAEMON}.state"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_workflow_infer_deps_smoke" \
  --db-path "${DB_PATH}" \
  --state-dir "${STATE_DIR}" \
  --tools none \
  --workflow-max-inflight-per-workflow 1

agentd_smoke_wait_health "${DAEMON_URL}"

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<'PY'
import json

# Intentionally omit depends_on for B; it references task A in templates.
# With infer_depends_on=true, the server adds depends_on=["A"] deterministically.
tasks = [
  {
    "task_id": "A",
    "kind": "delay",
    "delay_ms": 250,
    "priority": -10,
    "result": {"assistant_text": "Alpha"},
  },
  {
    "task_id": "B",
    "kind": "delay",
    "delay_ms": 0,
    "priority": 10,
    "result": {"assistant_text": "B got ${task.A.json:/assistant_text}"},
    "expect": {"assistant_text_contains": "Alpha"},
  },
]

payload = {
  "infer_depends_on": True,
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
if w.get("status") != "done":
  print("expected workflow status done", w, file=sys.stderr)
  raise SystemExit(1)
tasks = obj.get("tasks") or []
by_id = {t.get("task_id"): t for t in tasks if isinstance(t, dict)}
for tid in ("A","B"):
  if by_id.get(tid, {}).get("status") != "done":
    print("task not done", tid, by_id.get(tid), file=sys.stderr)
    raise SystemExit(1)

result = obj.get("result") or {}
by_task = (result.get("results_by_task") or {})
b = by_task.get("B") or {}
txt = (b.get("assistant_text") or "").strip()
if "B got Alpha" not in txt:
  print("unexpected B assistant_text", txt, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_workflow_infer_deps_smoke OK"

