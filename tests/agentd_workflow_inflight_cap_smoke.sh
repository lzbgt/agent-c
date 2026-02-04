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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_workflow_inflight_cap_smoke" \
  --tools none \
  --workflow-concurrency 2 \
  --workflow-max-inflight-per-workflow 1

agentd_smoke_wait_health "${DAEMON_URL}"

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<'PY'
import json
tasks = [
  {"task_id":"A","kind":"delay","delay_ms":650,"result":{"assistant_text":"A"}},
  {"task_id":"B","kind":"delay","delay_ms":650,"result":{"assistant_text":"B"}},
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
for _ in $(seq 1 220); do
  final="$(curl -fsS --noproxy "*" --max-time 5 \
    "${DAEMON_URL}/api/v1/workflow?workflow_id=${workflow_id}&include_tasks=1")"
  if python3 - <<PY >/dev/null 2>&1
import json, sys
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
for tid in ("A","B"):
  if by_id.get(tid, {}).get("status") != "done":
    print("task not done", tid, by_id.get(tid), file=sys.stderr)
    raise SystemExit(1)

def i64(tid, key):
  v = by_id.get(tid, {}).get(key, 0)
  return int(v or 0)

sA, fA = i64("A","started_unix_ms"), i64("A","finished_unix_ms")
sB, fB = i64("B","started_unix_ms"), i64("B","finished_unix_ms")
if not (sA > 0 and sB > 0 and fA > 0 and fB > 0):
  print("missing timing fields", {"A":by_id.get("A"),"B":by_id.get("B")}, file=sys.stderr)
  raise SystemExit(1)

# Fairness/budget correctness: with max_inflight_per_workflow=1, the delay tasks must not overlap.
skew = 25  # ms tolerance for coarse timestamps
if sA <= sB:
  if sB < fA - skew:
    print("tasks overlapped but should not (A then B)", {"sA":sA,"fA":fA,"sB":sB,"fB":fB}, file=sys.stderr)
    raise SystemExit(1)
  if sB - sA < 450:
    print("expected near-serial start gap (A then B)", {"sA":sA,"sB":sB}, file=sys.stderr)
    raise SystemExit(1)
else:
  if sA < fB - skew:
    print("tasks overlapped but should not (B then A)", {"sA":sA,"fA":fA,"sB":sB,"fB":fB}, file=sys.stderr)
    raise SystemExit(1)
  if sA - sB < 450:
    print("expected near-serial start gap (B then A)", {"sA":sA,"sB":sB}, file=sys.stderr)
    raise SystemExit(1)
PY

