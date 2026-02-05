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
NAME="agentd_workflow_stats_budget_pressure_smoke"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}" \
  --tools host \
  --host-policy full \
  --workflow-concurrency 1

agentd_smoke_wait_health "${DAEMON_URL}"

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
req = {
  "workflow_limits": {
    "max_tool_calls_total": 2
  },
  "tasks": [
    {
      "task_id":"A",
      "kind":"memory_put",
      "memory_put": {
        "path":"STRUCTURED.md",
        "checkpoint": True,
        "entries":[{"key":"k_budget_pressure","value":"v1"}]
      }
    },
    {
      "task_id":"B",
      "kind":"delay",
      "depends_on":["A"],
      "delay_ms": 1500,
      "result": {"assistant_text":"ok"}
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

# Wait until B is running so the workflow is still queued|running when we query stats.
snap=""
for _ in $(seq 1 260); do
  snap="$(curl -fsS --noproxy "*" --max-time 5 \
    "${DAEMON_URL}/api/v1/workflow?workflow_id=${workflow_id}&include_tasks=1&include_results=0")"
  if python3 - <<PY >/dev/null 2>&1
import json
obj = json.loads(r'''${snap}''')
tasks = obj.get("tasks") or []
by = {t.get("task_id"): t for t in tasks if isinstance(t, dict)}
raise SystemExit(0 if by.get("B", {}).get("status") == "running" else 1)
PY
  then
    break
  fi
  sleep 0.05
done

stats="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/workflow/stats?include_budget_pressure=1&include_budget_workflows=1&budget_workflow_limit=128")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${stats}''')
if not obj.get("ok"):
  print("stats not ok", obj, file=sys.stderr)
  raise SystemExit(1)
bp = obj.get("budget_pressure") or {}
if not isinstance(bp, dict) or int(bp.get("workflows_scanned") or 0) < 1:
  print("missing/invalid budget_pressure", obj, file=sys.stderr)
  raise SystemExit(1)
tc = bp.get("tool_calls") or {}
if int(tc.get("workflows_limited") or 0) < 1:
  print("expected at least 1 budgeted workflow", bp, file=sys.stderr)
  raise SystemExit(1)
ws = bp.get("workflows") or []
if not isinstance(ws, list) or not ws:
  print("expected budget_pressure.workflows sample", bp, file=sys.stderr)
  raise SystemExit(1)
found = False
for row in ws:
  if not isinstance(row, dict):
    continue
  if row.get("workflow_id") != "${workflow_id}":
    continue
  rem = row.get("remaining") or {}
  if int(rem.get("max_tool_calls_total") or -1) != 1:
    print("expected remaining.max_tool_calls_total == 1", row, file=sys.stderr)
    raise SystemExit(1)
  found = True
if not found:
  print("workflow not present in budget samples", ws, file=sys.stderr)
  raise SystemExit(1)
PY

echo "ok: ${NAME}"

