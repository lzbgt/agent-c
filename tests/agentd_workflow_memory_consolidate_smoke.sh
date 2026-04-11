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
NAME="agentd_workflow_memory_consolidate_smoke"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}" \
  --tools host \
  --host-policy full \
  --workflow-concurrency 1 \
  --workflow-max-inflight-per-workflow 1

agentd_smoke_wait_health "${DAEMON_URL}"

STATE_DIR="${LOG_DIR}/${NAME}_${PORT_DAEMON}.state"
mem_root="${STATE_DIR}/memory"
mkdir -p "${mem_root}"

today="$(date +%F)"
cat > "${mem_root}/${today}.md" <<'EOF'
### note
- @mem fact wf.consolidate.test = Consolidation must be triggerable from durable workflow tasks
EOF
cat > "${mem_root}/MEMORY.md" <<'EOF'
# Core memory
- @mem fact wf.consolidate.core = Workflow consolidation scans core memory markers
EOF
mkdir -p "${mem_root}/sessions"
cat > "${mem_root}/sessions/workflow-session.md" <<'EOF'
# Session memory
- @mem fact wf.consolidate.session = Workflow consolidation scans bounded session markers
EOF
cat > "${mem_root}/STRUCTURED.md" <<'EOF'
# Structured Memory

<!-- AGENT_MEMORY_V1_BEGIN -->
{"schema":"agent_memory_v2","items":{"wf.structured.machine.seed":{"kind":"fact","value":"@mem fact wf.structured.machine.promoted = Machine block marker must not promote","status":"active","updated_utc":"2026-04-01T00:00:00Z","observed_utc":"2026-04-01T00:00:00Z","sources":["seed"]}}}
<!-- AGENT_MEMORY_V1_END -->

<!-- AGENT_MEMORY_V1_NOTES_BEGIN -->
- @mem fact wf.consolidate.structured = Workflow consolidation scans structured freeform notes
<!-- AGENT_MEMORY_V1_NOTES_END -->
EOF

TRACE_ID="wf_mem_consolidate_smoke"

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(
    TRACE_ID="${TRACE_ID}" python3 - <<'PY'
import json, os
trace_id = os.environ["TRACE_ID"]

tasks = [
  {
    "task_id": "C",
    "kind": "memory_consolidate",
    "memory_consolidate": {
      "daily_days": 1,
      "session_days": 1,
      "max_session_files": 4,
      "max_file_bytes": 1048576,
      "max_entries": 64,
      "include_core": True,
      "include_daily": True,
      "include_session": True,
      "include_structured": True,
    },
    "max_attempts": 1,
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
for _ in $(seq 1 220); do
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
by = (res.get("results_by_task") or {})
c = by.get("C") or {}
if c.get("kind") != "memory_consolidate":
  print("expected C kind=memory_consolidate", c, file=sys.stderr)
  raise SystemExit(1)
if c.get("ok") is not True:
  print("expected C ok true", c, file=sys.stderr)
  raise SystemExit(1)
rep = c.get("report") or {}
if not isinstance(rep, dict):
  print("expected report object", rep, file=sys.stderr)
  raise SystemExit(1)
layers = {f.get("layer") for f in rep.get("files_scanned") or [] if isinstance(f, dict)}
for layer in ("core", "daily", "session", "structured"):
  if layer not in layers:
    print("expected scanned layer", layer, "in", layers, rep, file=sys.stderr)
    raise SystemExit(1)
PY

STRUCTURED="${mem_root}/STRUCTURED.md"
if [[ ! -f "${STRUCTURED}" ]]; then
  echo "missing structured memory file: ${STRUCTURED}" >&2
  exit 1
fi
for key in wf.consolidate.test wf.consolidate.core wf.consolidate.session wf.consolidate.structured; do
  if ! rg -n "${key//./\\.}" "${STRUCTURED}" >/dev/null 2>&1; then
    echo "expected ${key} in STRUCTURED.md" >&2
    exit 1
  fi
done
STRUCTURED="${STRUCTURED}" python3 - <<'PY'
import json, os, re, sys
text = open(os.environ["STRUCTURED"], encoding="utf-8").read()
m = re.search(r"<!-- AGENT_MEMORY_V1_BEGIN -->\n(.*?)\n<!-- AGENT_MEMORY_V1_END -->", text, re.S)
if not m:
  print("missing structured machine block", file=sys.stderr)
  raise SystemExit(1)
items = (json.loads(m.group(1)).get("items") or {})
if "wf.structured.machine.seed" not in items:
  print("missing seeded structured machine item", sorted(items), file=sys.stderr)
  raise SystemExit(1)
if "wf.structured.machine.promoted" in items:
  print("structured machine block marker was promoted", items["wf.structured.machine.promoted"], file=sys.stderr)
  raise SystemExit(1)
sources = (items.get("wf.consolidate.structured") or {}).get("sources") or []
if "structured:STRUCTURED.md" not in sources:
  print("expected stable structured source for workflow structured marker", sources, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_workflow_memory_consolidate_smoke OK"
