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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_workflow_memory_structured_query_smoke" \
  --tools host \
  --host-policy full

agentd_smoke_wait_health "${DAEMON_URL}"

TRACE_ID="wf_mem_structured_query_smoke"

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
      "path": "STRUCTURED.md",
      "entries": [
        {"key": "wf.test.q1", "kind": "fact", "value": "Autonomy comes from durable scheduling + memory."},
        {"key": "wf.test.q2", "kind": "preference", "value": "Prefer deterministic joins over ad-hoc heuristics."},
        {"key": "wf.test.q3", "kind": "fact", "value": "Newest fact for ordering test."}
      ],
      "checkpoint": False
    },
    "max_attempts": 1
  },
  {
    "task_id": "Q",
    "kind": "memory_structured_query",
    "depends_on": ["P"],
    "memory_structured_query": {
      "path": "STRUCTURED.md",
      "key": "wf.test.q1",
      "status": "active",
      "include_sources": False,
      "include_versions": False,
      "limit": 10
    },
    "expect": {
      "json_pointer_exists": ["/memory_structured_query_response/data/results/0/record/value"],
      "json_pointer_regex": [{"pointer": "/memory_structured_query_response/data/results/0/record/value", "regex": "durable scheduling"}]
    },
    "max_attempts": 1
  },
  {
    "task_id": "T",
    "kind": "memory_structured_query",
    "depends_on": ["P"],
    "memory_structured_query": {
      "path": "STRUCTURED.md",
      "source_contains": "trace:" + trace_id + ":P",
      "kinds": ["fact"],
      "status": "active",
      "include_sources": False,
      "include_versions": False,
      "limit": 10
    },
    "expect": {
      "json_pointer_exists": ["/memory_structured_query_response/data/results/0/key"],
      "json_pointer_regex": [{"pointer": "/memory_structured_query_response/data/results/0/key", "regex": "^wf\\.test\\.q1$"}]
    },
    "max_attempts": 1
  },
  {
    "task_id": "U",
    "kind": "memory_structured_query",
    "depends_on": ["P"],
    "memory_structured_query": {
      "path": "STRUCTURED.md",
      "key_prefix": "wf.test.",
      "kinds": ["fact"],
      "status": "active",
      "include_sources": False,
      "include_versions": False,
      "order_by": "updated_desc",
      "limit": 2
    },
    "expect": {
      "json_pointer_exists": ["/memory_structured_query_response/data/results/0/key"]
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
q = by.get("Q") or {}
if q.get("kind") != "memory_structured_query" or q.get("ok") is not True:
  print("expected Q ok true memory_structured_query", q, file=sys.stderr)
  raise SystemExit(1)
resp = q.get("memory_structured_query_response") or {}
data = resp.get("data") or {}
results = data.get("results") or []
if not results:
  print("expected at least 1 result", q, file=sys.stderr)
  raise SystemExit(1)
if results[0].get("key") != "wf.test.q1":
  print("unexpected key", results[0], file=sys.stderr)
  raise SystemExit(1)

t = by.get("T") or {}
if t.get("kind") != "memory_structured_query" or t.get("ok") is not True:
  print("expected T ok true memory_structured_query", t, file=sys.stderr)
  raise SystemExit(1)
resp = t.get("memory_structured_query_response") or {}
data = resp.get("data") or {}
results = data.get("results") or []
if not results:
  print("expected at least 1 result for T", t, file=sys.stderr)
  raise SystemExit(1)
if results[0].get("key") != "wf.test.q1":
  print("unexpected T key", results[0], file=sys.stderr)
  raise SystemExit(1)

u = by.get("U") or {}
if u.get("kind") != "memory_structured_query" or u.get("ok") is not True:
  print("expected U ok true memory_structured_query", u, file=sys.stderr)
  raise SystemExit(1)
resp = u.get("memory_structured_query_response") or {}
data = resp.get("data") or {}
results = data.get("results") or []
if len(results) < 2:
  print("expected at least 2 results for U", u, file=sys.stderr)
  raise SystemExit(1)
keys = [r.get("key","") for r in results]
if "wf.test.q1" not in keys or "wf.test.q3" not in keys:
  print("expected wf.test.q1 and wf.test.q3 keys in U", keys, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_workflow_memory_structured_query_smoke OK"
