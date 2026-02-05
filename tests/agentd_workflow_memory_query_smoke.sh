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
NAME="agentd_workflow_memory_query_smoke"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}" \
  --tools host \
  --host-policy full

agentd_smoke_wait_health "${DAEMON_URL}"

TRACE_ID="wf_mem_query_task_smoke"

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(
    TRACE_ID="${TRACE_ID}" python3 - <<'PY'
import json, os, time

trace_id = os.environ["TRACE_ID"]
now_ms = int(time.time() * 1000)

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
        {"key": "wf.q.corr", "kind": "fact", "value": "v1"},
        {"key": "wf.q.alpha", "kind": "fact", "value": "${task.A.assistant_text}"},
      ],
      "checkpoint": True,
      "keep_checkpoints": 5,
    },
    "max_attempts": 1,
  },
  {
    "task_id": "Q",
    "kind": "memory_query",
    "depends_on": ["M"],
    "memory_query": {
      "since_utc_ms": now_ms - 5000,
      "until_utc_ms": now_ms + 60000,
      "structured_path": "STRUCTURED.md",
      "key_prefix": "wf.q.",
      "limit": 50,
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
res = obj.get("result") or {}
by = res.get("results_by_task") or {}
q = by.get("Q") or {}
if q.get("kind") != "memory_query":
  print("expected Q kind=memory_query", q, file=sys.stderr)
  raise SystemExit(1)
if q.get("ok") is not True:
  print("expected Q ok true", q, file=sys.stderr)
  raise SystemExit(1)
if q.get("structured_path_filter") != "STRUCTURED.md":
  print("expected structured_path_filter echo", q.get("structured_path_filter"), file=sys.stderr)
  raise SystemExit(1)
if q.get("key_prefix") != "wf.q.":
  print("expected key_prefix echo", q.get("key_prefix"), file=sys.stderr)
  raise SystemExit(1)
entries = q.get("entries") or []
keys = sorted({e.get("key") for e in entries if isinstance(e, dict)})
if keys != ["wf.q.alpha", "wf.q.corr"]:
  print("unexpected Q entries keys", keys, file=sys.stderr)
  raise SystemExit(1)
ebk = q.get("entries_by_key") or {}
if not isinstance(ebk, dict) or "wf.q.alpha" not in ebk or "wf.q.corr" not in ebk:
  print("expected entries_by_key mapping", ebk, file=sys.stderr)
  raise SystemExit(1)
alpha = ebk.get("wf.q.alpha") or {}
if not isinstance(alpha, dict) or alpha.get("value") != "alpha":
  print("unexpected wf.q.alpha record", alpha, file=sys.stderr)
  raise SystemExit(1)
ck = q.get("checkpoint") or {}
if not isinstance(ck, dict) or not ck.get("checkpoint_path"):
  print("expected checkpoint metadata", ck, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_workflow_memory_query_smoke OK"

