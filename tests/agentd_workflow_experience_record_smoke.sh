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

PORT_DAEMON="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"
NAME="agentd_workflow_experience_record_smoke"
LOG_DIR="$(agentd_smoke_log_dir)"
STATE_DIR="${LOG_DIR}/${NAME}_${PORT_DAEMON}.state"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}" \
  --tools none \
  --state-dir "${STATE_DIR}"

agentd_smoke_wait_health "${DAEMON_URL}"

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(
    python3 - <<'PY'
import json
tasks = [
  {"task_id":"A","kind":"delay","delay_ms":0,"result":{"assistant_text":"learned proof","score":7}},
  {"task_id":"L","kind":"experience_record","depends_on":["A"],
   "experience_record":{"label":"smoke/closed-loop","task_ids":["A"],"reward":0.75,
                        "metadata":{"suite":"agentd_workflow_experience_record_smoke"}}}
]
print(json.dumps({"tasks": tasks, "allow_sessions": False}))
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
import json
obj = json.loads(r'''${final}''')
st = (obj.get("workflow") or {}).get("status")
raise SystemExit(0 if st in ("done","error","cancelled") else 1)
PY
  then
    break
  fi
  sleep 0.05
done

EXPERIENCE_PATH="${STATE_DIR}/rl/experience_records.jsonl"
python3 - <<PY
import json, pathlib, sys
obj = json.loads(r'''${final}''')
if not obj.get("ok"):
    print("workflow get failed", obj, file=sys.stderr)
    raise SystemExit(1)
w = obj.get("workflow") or {}
if w.get("status") != "done":
    print("expected workflow status done", w, file=sys.stderr)
    raise SystemExit(1)
by = (obj.get("result") or {}).get("results_by_task") or {}
l = by.get("L") or {}
if l.get("kind") != "experience_record" or l.get("ok") is not True:
    print("expected experience_record result", l, file=sys.stderr)
    raise SystemExit(1)
if l.get("path") != "rl/experience_records.jsonl" or abs(float(l.get("reward", 0)) - 0.75) > 0.0001:
    print("unexpected experience result surface", l, file=sys.stderr)
    raise SystemExit(1)
p = pathlib.Path(r'''${EXPERIENCE_PATH}''')
if not p.exists():
    print("missing experience jsonl", p, file=sys.stderr)
    raise SystemExit(1)
records = [json.loads(line) for line in p.read_text(encoding="utf-8").splitlines() if line.strip()]
if len(records) != 1:
    print("expected one record", records, file=sys.stderr)
    raise SystemExit(1)
r = records[0]
if r.get("schema") != "agentd_experience_record_v1" or r.get("label") != "smoke/closed-loop":
    print("unexpected record identity", r, file=sys.stderr)
    raise SystemExit(1)
if "A" not in (r.get("source_results_by_task") or {}):
    print("missing source task result", r, file=sys.stderr)
    raise SystemExit(1)
if abs(float(r.get("reward", 0)) - 0.75) > 0.0001:
    print("unexpected reward", r, file=sys.stderr)
    raise SystemExit(1)
PY

export_resp="$(curl -fsS --noproxy "*" --max-time 5 \
  "${DAEMON_URL}/api/v1/rl/experience_records?label=smoke%2Fclosed-loop&min_reward=0.7&limit=10")"
python3 - <<PY
import json, sys
obj = json.loads(r'''${export_resp}''')
if not obj.get("ok"):
    print("experience export failed", obj, file=sys.stderr)
    raise SystemExit(1)
records = obj.get("records") or []
if len(records) != 1:
    print("expected one exported record", obj, file=sys.stderr)
    raise SystemExit(1)
r = records[0]
if r.get("workflow_id") != r'''${workflow_id}''' or r.get("label") != "smoke/closed-loop":
    print("unexpected exported record", r, file=sys.stderr)
    raise SystemExit(1)
if abs(float(r.get("reward", 0)) - 0.75) > 0.0001:
    print("unexpected exported reward", r, file=sys.stderr)
    raise SystemExit(1)
if obj.get("path") != "rl/experience_records.jsonl" or obj.get("schema") != "agentd_experience_record_v1":
    print("unexpected export metadata", obj, file=sys.stderr)
    raise SystemExit(1)
PY

echo "agentd_workflow_experience_record_smoke OK"
