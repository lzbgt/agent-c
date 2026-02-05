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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_workflow_aggregate_quorum_hashes_object_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

submit_resp="$(curl -g -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json

# Same semantic object, different key insertion order. Canonicalization should make them hash-identical.
tasks = [
  {"task_id":"A","kind":"delay","delay_ms":0,"result":{"obj":{"a":1,"b":2}}},
  {"task_id":"B","kind":"delay","delay_ms":0,"result":{"obj":{"b":2,"a":1}}},
  {"task_id":"C","kind":"delay","delay_ms":0,"result":{"obj":{"a":1,"b":2}}},
  {"task_id":"J","kind":"aggregate","depends_on":["A","B","C"],
   "aggregate":{"mode":"quorum_hashes","task_ids":["A","B","C"],"quorum":3,"pointers":["/obj"]}}
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

res = obj.get("result") or {}
by = res.get("results_by_task") or {}
j = by.get("J") or {}
if j.get("mode") != "quorum_hashes" or j.get("ok") is not True:
  print("unexpected join result", j, file=sys.stderr)
  raise SystemExit(1)

checks = j.get("checks") or []
by_ptr = {c.get("ptr"): c for c in checks if isinstance(c, dict)}
c = by_ptr.get("/obj") or {}
if c.get("ok") is not True:
  print("expected /obj quorum ok", c, file=sys.stderr)
  raise SystemExit(1)

chosen = c.get("chosen") or ""
if not (isinstance(chosen, str) and chosen.startswith("sha256:") and len(chosen) == 71):
  print("unexpected chosen token", chosen, file=sys.stderr)
  raise SystemExit(1)

if int(c.get("chosen_count", 0)) != 3:
  print("expected chosen_count=3", c, file=sys.stderr)
  raise SystemExit(1)

vbt = c.get("values_by_task") or {}
for tid in ("A","B","C"):
  if (vbt.get(tid) or "") != chosen:
    print("expected all task values to match chosen", tid, vbt.get(tid), chosen, file=sys.stderr)
    raise SystemExit(1)
PY

echo "agentd_workflow_aggregate_quorum_hashes_object_smoke OK"

