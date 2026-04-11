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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_workflow_memory_put_smoke" \
  --tools host \
  --host-policy full

agentd_smoke_wait_health "${DAEMON_URL}"

TRACE_ID="wf_mem_put_smoke"

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
        {
          "key": "wf.test.alpha",
          "kind": "fact",
          "value": "${task.A.assistant_text}",
          "sources": ["workflow-smoke:manual-source"],
          "observed_utc": "2026-04-02T03:04:05Z",
          "valid_from": "2026-04-01T00:00:00Z",
          "supersedes": ["legacy:wf.test.alpha"]
        }
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
for _ in $(seq 1 200); do
  final="$(curl -fsS --noproxy "*" --max-time 5 \
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
by = (res.get("results_by_task") or {})
m = by.get("M") or {}
if m.get("kind") != "memory_put":
  print("expected M kind=memory_put", m, file=sys.stderr)
  raise SystemExit(1)
if m.get("ok") is not True:
  print("expected M ok true", m, file=sys.stderr)
  raise SystemExit(1)
PY

STATE_DIR="${LOG_DIR}/agentd_workflow_memory_put_smoke_${PORT_DAEMON}.state"
STRUCTURED="${STATE_DIR}/memory/STRUCTURED.md"
if [[ ! -f "${STRUCTURED}" ]]; then
  echo "missing structured memory file: ${STRUCTURED}" >&2
  exit 1
fi

TRACE_ID="${TRACE_ID}" STRUCTURED="${STRUCTURED}" python3 - <<PY
import json, os, re, sys

trace_id = os.environ.get("TRACE_ID","wf_mem_put_smoke")
path = os.environ["STRUCTURED"]

data = open(path, "rb").read().decode("utf-8", errors="replace")
begin = "<!-- AGENT_MEMORY_V1_BEGIN -->"
end = "<!-- AGENT_MEMORY_V1_END -->"
a = data.find(begin)
b = data.find(end)
if a < 0 or b < 0 or b <= a:
  print("missing memory block markers", file=sys.stderr)
  raise SystemExit(1)
body = data[a + len(begin):b].strip()
doc = json.loads(body)
if doc.get("schema") != "agent_memory_v2":
  print("unexpected memory schema", doc.get("schema"), file=sys.stderr)
  raise SystemExit(1)
items = doc.get("items") or {}
rec = items.get("wf.test.alpha") or {}
if rec.get("value") != "alpha":
  print("expected wf.test.alpha value alpha, got", rec.get("value"), file=sys.stderr)
  raise SystemExit(1)
if rec.get("observed_utc") != "2026-04-02T03:04:05Z":
  print("expected explicit observed_utc", rec, file=sys.stderr)
  raise SystemExit(1)
if rec.get("valid_from") != "2026-04-01T00:00:00Z":
  print("expected explicit valid_from", rec, file=sys.stderr)
  raise SystemExit(1)
supersedes = rec.get("supersedes") or []
if "legacy:wf.test.alpha" not in supersedes:
  print("expected explicit supersedes evidence", supersedes, file=sys.stderr)
  raise SystemExit(1)
sources = rec.get("sources") or []
if not isinstance(sources, list) or not sources:
  print("expected non-empty sources", sources, file=sys.stderr)
  raise SystemExit(1)
if "workflow-smoke:manual-source" not in sources:
  print("expected manual source array to survive submit sanitizer", sources, file=sys.stderr)
  raise SystemExit(1)
needle = f" task:M trace:{trace_id}:M"
ok = any(isinstance(s, str) and ("workflow:" in s) and needle in s for s in sources)
if not ok:
  print("expected sources to include workflow correlation", sources, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_workflow_memory_put_smoke OK"
