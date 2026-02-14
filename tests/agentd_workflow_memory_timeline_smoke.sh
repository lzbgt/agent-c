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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_workflow_memory_timeline_smoke" \
  --tools host \
  --host-policy full

agentd_smoke_wait_health "${DAEMON_URL}"

TRACE_ID="wf_mem_timeline_smoke"

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(
    TRACE_ID="${TRACE_ID}" python3 - <<'PY'
import json, os

trace_id = os.environ["TRACE_ID"]

tasks = [
  {
    "task_id": "P",
    "kind": "memory_put",
    "memory_put": {
      "path": "MEMORY.md",
      "entries": [
        {"key": "wf.test.timeline", "kind": "fact", "value": "Timeline entry must persist."}
      ],
      "checkpoint": False
    },
    "max_attempts": 1
  },
  {
    "task_id": "S",
    "kind": "memory_search",
    "depends_on": ["P"],
    "memory_search": {
      "query": "Timeline entry must persist",
      "max_results": 5,
      "daily_days": 0,
      "case_sensitive": False,
      "use_index": True
    },
    "expect": {
      "json_pointer_exists": ["/memory_search_response/data/results/0/citation"]
    },
    "max_attempts": 1
  },
  {
    "task_id": "T",
    "kind": "memory_timeline",
    "depends_on": ["S"],
    "memory_timeline": {
      "citation": "${task.S.json:/memory_search_response/data/results/0/citation}",
      "context_lines": 2,
      "max_chars": 2000
    },
    "expect": {
      "json_pointer_regex": [
        {"pointer": "/memory_timeline_response/data/text", "regex": "Timeline entry must persist"}
      ]
    },
    "max_attempts": 1
  }
]

print(json.dumps({"tasks": tasks, "trace_id": trace_id}))
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
for _ in $(seq 1 240); do
  final="$(curl -fsS --noproxy "*" --max-time 5 \
    "${DAEMON_URL}/api/v1/workflow?workflow_id=${workflow_id}&include_tasks=1&include_results=1")"
  if python3 - <<PY >/dev/null 2>&1
import json
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
res = obj.get("result") or {}
by = res.get("results_by_task") or {}
t = by.get("T") or {}
if t.get("kind") != "memory_timeline" or t.get("ok") is not True:
  print("expected T ok true memory_timeline", t, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_workflow_memory_timeline_smoke OK"
