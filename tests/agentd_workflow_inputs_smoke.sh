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

DB_PATH="${LOG_DIR}/agentd_workflow_inputs_smoke_${PORT_DAEMON}.sqlite"
STATE_DIR="${LOG_DIR}/agentd_workflow_inputs_smoke_${PORT_DAEMON}.state"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_workflow_inputs_smoke" \
  --db-path "${DB_PATH}" \
  --state-dir "${STATE_DIR}" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<'PY'
import json

tasks = [
  {
    "task_id": "A",
    "kind": "delay",
    "delay_ms": 0,
    "result": {"assistant_text": "Alpha", "obj": {"n": 1, "s": "x"}},
  },
  {
    "task_id": "B",
    "kind": "delay",
    "depends_on": ["A"],
    "delay_ms": 0,
    "result": {"assistant_text": "${input.greet}"},
    "expect": {"assistant_text_contains": "Hello Alpha"},
  },
  {
    "task_id": "C",
    "kind": "delay",
    "depends_on": ["A"],
    "delay_ms": 0,
    "result": {"obj": {"$ref": "input.alpha_obj2"}},
    "expect": {"json_pointer_equals": {"pointer": "/obj/n", "value": 1}},
  },
  {
    "task_id": "D",
    "kind": "delay",
    "depends_on": ["A"],
    "delay_ms": 0,
    "result": {"assistant_text": "${input.alpha_obj2.json:/s}"},
    "expect": {"assistant_text_contains": "x"},
  },
]

payload = {
  "inputs": {
    "greet": "Hello ${task.A.json:/assistant_text}",
    "alpha_obj": {"$ref": "task.A.json:/obj"},
    "alpha_obj2": {"$ref": "input.alpha_obj"},
  },
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
for _ in $(seq 1 160); do
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
for tid in ("A","B","C","D"):
  if by_id.get(tid, {}).get("status") != "done":
    print("task not done", tid, by_id.get(tid), file=sys.stderr)
    raise SystemExit(1)

result = obj.get("result") or {}
by_task = (result.get("results_by_task") or {})

def r(tid):
  return by_task.get(tid) or {}

if (r("A").get("assistant_text") or "").strip() != "Alpha":
  print("unexpected A assistant_text", r("A"), file=sys.stderr)
  raise SystemExit(1)
if "Hello Alpha" not in ((r("B").get("assistant_text") or "").strip()):
  print("unexpected B assistant_text", r("B"), file=sys.stderr)
  raise SystemExit(1)

obj_c = r("C").get("obj") or {}
if obj_c.get("n") != 1 or obj_c.get("s") != "x":
  print("unexpected C obj", obj_c, file=sys.stderr)
  raise SystemExit(1)

if (r("D").get("assistant_text") or "").strip() != "x":
  print("unexpected D assistant_text", r("D"), file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_workflow_inputs_smoke OK"

