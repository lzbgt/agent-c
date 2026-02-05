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

DB_PATH="${LOG_DIR}/agentd_trace_memory_correlate_smoke_${PORT_DAEMON}.sqlite"
STATE_DIR="${LOG_DIR}/agentd_trace_memory_correlate_smoke_${PORT_DAEMON}.state"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_trace_memory_correlate_smoke" \
  --db-path "${DB_PATH}" \
  --state-dir "${STATE_DIR}" \
  --tools host \
  --host-policy full

agentd_smoke_wait_health "${DAEMON_URL}"

TRACE_ID="trace_mem_$(date +%s)_$RANDOM"

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(TRACE_ID="${TRACE_ID}" python3 - <<'PY'
import json, os, time

trace_id = os.environ.get("TRACE_ID") or ""
now_ms = int(time.time() * 1000)

tasks = [
  {"task_id": "A", "kind": "delay", "delay_ms": 0, "result": {"assistant_text": "alpha"}},
  {
    "task_id": "M",
    "kind": "memory_put",
    "depends_on": ["A"],
    "memory_put": {
      "path": "STRUCTURED.md",
      "entries": [
        {"key": "wf.trace.mem.alpha", "kind": "fact", "value": "${task.A.assistant_text}"},
        {"key": "wf.trace.mem.fixed", "kind": "fact", "value": "v1"},
      ],
      "checkpoint": True,
      "keep_checkpoints": 5,
    },
  },
  {
    "task_id": "WAIT",
    "kind": "delay",
    "depends_on": ["M"],
    "delay_ms": 10,
    "result": {"assistant_text": "ok"},
  },
]

payload = {"trace_id": trace_id, "tasks": tasks}
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

trace_q="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/trace?trace_id=$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote('${TRACE_ID}'))
PY
)&limit=200&max_bytes=1048576&include_memory=1&memory_max_entries=200")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${trace_q}''')
if not obj.get("ok"):
  print("trace query failed", obj, file=sys.stderr)
  raise SystemExit(1)
mc = obj.get("memory_correlate") or {}
if mc.get("ok") is not True:
  print("expected memory_correlate ok true", mc, file=sys.stderr)
  raise SystemExit(1)
entries = mc.get("entries") or []
keys = {e.get("key") for e in entries if isinstance(e, dict)}
need = {"wf.trace.mem.alpha", "wf.trace.mem.fixed"}
if not need.issubset(keys):
  print("missing expected correlated keys", need, keys, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_trace_memory_correlate_smoke OK"

