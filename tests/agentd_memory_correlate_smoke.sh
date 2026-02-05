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
NAME="agentd_memory_correlate_smoke"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}" \
  --tools host \
  --host-policy full

agentd_smoke_wait_health "${DAEMON_URL}"

TRACE_ID="mem_corr_smoke"
SINCE_MS="$(python3 - <<PY
import time
print(int(time.time()*1000) - 5000)
PY
)"

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(
    TRACE_ID="${TRACE_ID}" python3 - <<'PY'
import json, os

trace_id = os.environ["TRACE_ID"]

tasks = [
  {
    "task_id": "A",
    "kind": "delay",
    "delay_ms": 0,
    "result": {"assistant_text": "alpha"},
    "max_attempts": 1,
  },
  {
    "task_id": "M",
    "kind": "memory_put",
    "depends_on": ["A"],
    "memory_put": {
      "path": "STRUCTURED.md",
      "entries": [
        {"key": "wf.test.corr", "kind": "fact", "value": "v1"},
        {"key": "wf.test.alpha", "kind": "fact", "value": "${task.A.assistant_text}"},
      ],
      "checkpoint": True,
      "keep_checkpoints": 5,
    },
    "max_attempts": 1,
  },
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
if w.get("status") != "done":
  print("expected workflow status done", w, file=sys.stderr)
  raise SystemExit(1)
PY

UNTIL_MS="$(python3 - <<PY
import time
print(int(time.time()*1000) + 5000)
PY
)"

ck_list="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/memory/checkpoints?since_utc_ms=${SINCE_MS}&until_utc_ms=${UNTIL_MS}&limit=50")"

python3 - <<PY
import json, re, sys
obj = json.loads(r'''${ck_list}''')
if not obj.get("ok"):
  print("checkpoints not ok", obj, file=sys.stderr)
  raise SystemExit(1)
cps = obj.get("checkpoints") or []
if not cps:
  print("expected at least 1 checkpoint", obj, file=sys.stderr)
  raise SystemExit(1)
sha_re = re.compile(r"^[a-f0-9]{64}$")
hit = 0
for c in cps:
  if not sha_re.match(c.get("sha256","")):
    print("invalid sha256", c, file=sys.stderr)
    raise SystemExit(1)
  hit += 1
if hit < 1:
  raise SystemExit(1)
print("ok")
PY

corr="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/memory/correlate?trace_id=${TRACE_ID}&since_utc_ms=${SINCE_MS}&until_utc_ms=${UNTIL_MS}&max_entries=200")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${corr}''')
if not obj.get("ok"):
  print("correlate not ok", obj, file=sys.stderr)
  raise SystemExit(1)
entries = obj.get("entries") or []
keys = {e.get("key") for e in entries if isinstance(e, dict)}
if "wf.test.corr" not in keys:
  print("expected wf.test.corr in entries", entries, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

echo "agentd_memory_correlate_smoke OK"

