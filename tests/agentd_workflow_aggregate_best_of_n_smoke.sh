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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_workflow_aggregate_best_of_n_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

submit_resp="$(curl -g -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
tasks = [
  {"task_id":"A","kind":"delay","delay_ms":0,"result":{"candidate":{"score":1,"answer":"alpha"}}},
  {"task_id":"B","kind":"delay","delay_ms":0,"result":{"candidate":{"score":9,"answer":"bravo"}}},
  {"task_id":"C","kind":"delay","delay_ms":0,"result":{"candidate":{"score":5,"answer":"charlie"}}},
  {"task_id":"J","kind":"aggregate","depends_on":["A","B","C"],
   "aggregate":{"mode":"best_of_n","task_ids":["A","B","C"],
                "candidate_pointer":"/candidate","parse_json":False,
                "score_pointer":"/score","value_pointer":"/answer","maximize":True}}
]
print(json.dumps({"tasks": tasks}))
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
  final="$(curl -g -fsS --noproxy "*" --max-time 5 \
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
res = obj.get("result") or {}
by = res.get("results_by_task") or {}
j = by.get("J") or {}
if j.get("mode") != "best_of_n" or j.get("ok") is not True:
  print("unexpected J", j, file=sys.stderr)
  raise SystemExit(1)
if j.get("chosen_task_id") != "B":
  print("unexpected chosen_task_id", j.get("chosen_task_id"), file=sys.stderr)
  raise SystemExit(1)
if (j.get("assistant_text") or "").strip() != "bravo":
  print("unexpected assistant_text", j.get("assistant_text"), file=sys.stderr)
  raise SystemExit(1)
if float(j.get("chosen_score", -1)) != 9.0:
  print("unexpected chosen_score", j.get("chosen_score"), file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_workflow_aggregate_best_of_n_smoke OK"
