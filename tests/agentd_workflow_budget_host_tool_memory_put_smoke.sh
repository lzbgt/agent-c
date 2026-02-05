#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "usage: agentd_workflow_budget_host_tool_memory_put_smoke.sh <agentd_bin>" >&2
  exit 2
fi

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

PORT_DAEMON="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"
NAME="agentd_workflow_budget_host_tool_memory_put_smoke"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}" \
  --tools host \
  --host-policy full

agentd_smoke_wait_health "${DAEMON_URL}"

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
req = {
  "workflow_limits": {
    # Only enough budget for ONE host tool call (memory_put) across the entire workflow.
    "max_tool_calls_total": 1
  },
  "tasks": [
    {
      "task_id":"A",
      "kind":"memory_put",
      "memory_put": {
        "path":"STRUCTURED.md",
        "checkpoint": True,
        "entries":[
          {"key":"k1","value":"v1"}
        ]
      }
    },
    {
      "task_id":"B",
      "kind":"memory_put",
      "depends_on":["A"],
      "memory_put": {
        "path":"STRUCTURED.md",
        "checkpoint": True,
        "entries":[
          {"key":"k2","value":"v2"}
        ]
      }
    }
  ]
}
print(json.dumps(req))
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
for _ in $(seq 1 260); do
  final="$(curl -fsS --noproxy "*" --max-time 5 \
    "${DAEMON_URL}/api/v1/workflow?workflow_id=${workflow_id}&include_tasks=1&include_results=1")"
  if python3 - <<PY >/dev/null 2>&1
import json
obj = json.loads(r'''${final}''')
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
if w.get("status") != "cancelled":
  print("expected workflow status cancelled (budget exceeded)", w, file=sys.stderr)
  raise SystemExit(1)
if not str(w.get("error","")).startswith("workflow budget exceeded"):
  print("expected workflow error to mention budget exceeded", w, file=sys.stderr)
  raise SystemExit(1)

tasks = obj.get("tasks") or []
by_id = {t.get("task_id"): t for t in tasks if isinstance(t, dict)}
if by_id.get("A", {}).get("status") != "done":
  print("expected A done", by_id.get("A"), file=sys.stderr)
  raise SystemExit(1)
if by_id.get("B", {}).get("status") != "cancelled":
  print("expected B cancelled by budget", by_id.get("B"), file=sys.stderr)
  raise SystemExit(1)

res = obj.get("result") or {}
by_task = res.get("results_by_task") if isinstance(res.get("results_by_task"), dict) else {}
a = by_task.get("A") if isinstance(by_task.get("A"), dict) else {}
if int(a.get("tool_calls_total", 0) or 0) != 1:
  print("expected A.tool_calls_total == 1 (host tool charged)", a, file=sys.stderr)
  raise SystemExit(1)
PY

echo "ok: ${NAME}"

