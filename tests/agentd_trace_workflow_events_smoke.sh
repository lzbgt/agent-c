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

DB_PATH="${LOG_DIR}/agentd_trace_workflow_events_smoke_${PORT_DAEMON}.sqlite"
STATE_DIR="${LOG_DIR}/agentd_trace_workflow_events_smoke_${PORT_DAEMON}.state"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_trace_workflow_events_smoke" \
  --db-path "${DB_PATH}" \
  --state-dir "${STATE_DIR}" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

TRACE_ID="trace_workflow_$(date +%s)_$RANDOM"
DURABLE_WF_ID="wf_durable_trace_${TRACE_ID}"
EDGE_WF_ID="wf_edge_trace_${TRACE_ID}"

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(DURABLE_WF_ID="${DURABLE_WF_ID}" TRACE_ID="${TRACE_ID}" python3 - <<'PY'
import json, os

payload = {
  "workflow_id": os.environ.get("DURABLE_WF_ID") or "",
  "trace_id": os.environ.get("TRACE_ID") or "",
  "tasks": [
    {"task_id": "A", "kind": "delay", "delay_ms": 0, "result": {"assistant_text": "ok"}},
  ],
}
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

# Wait for durable workflow to complete so workflow_events are populated beyond workflow_created.
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
if w.get("workflow_id") != "${DURABLE_WF_ID}":
  print("workflow_id mismatch", w.get("workflow_id"), file=sys.stderr)
  raise SystemExit(1)
if w.get("trace_id") != "${TRACE_ID}":
  print("trace_id mismatch", w.get("trace_id"), file=sys.stderr)
  raise SystemExit(1)
PY

# Create an edge workflow and correlate edge_workflow_events via an edge task with the same trace_id.
edge_submit="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(EDGE_WF_ID="${EDGE_WF_ID}" python3 - <<'PY'
import json, os

payload = {
  "workflow_id": os.environ.get("EDGE_WF_ID") or "",
  "goal": "trace smoke",
  "priority": 0,
  "steps": [
    {
      "step_id": "s1",
      "kind": "run_agent",
      "target": {"node_id": "node_trace_dummy"},
      "payload": {"prompt": "hi"},
    }
  ],
}
print(json.dumps(payload))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/workflow/submit")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${edge_submit}''')
if not obj.get("ok"):
  print("edge workflow submit failed", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("workflow_id") != "${EDGE_WF_ID}":
  print("edge workflow_id mismatch", obj.get("workflow_id"), file=sys.stderr)
  raise SystemExit(1)
PY

edge_assign="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(EDGE_WF_ID="${EDGE_WF_ID}" TRACE_ID="${TRACE_ID}" python3 - <<'PY'
import json, os, time
now = int(time.time() * 1000)
payload = {
  "node_id": "node_trace_dummy",
  "task_id": os.environ.get("EDGE_WF_ID") or "",
  "step_id": "s1",
  "idempotency_key": "k_trace_wf",
  "mode": "agent",
  "deadline_utc_ms": now + 60000,
  "attempt": 0,
  "payload": {"prompt": "ping"},
  "trace": {"trace_id": os.environ.get("TRACE_ID") or ""},
}
print(json.dumps(payload))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/task/assign")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${edge_assign}''')
if not obj.get("ok"):
  print("edge task assign failed", obj, file=sys.stderr)
  raise SystemExit(1)
PY

edge_msg="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(EDGE_WF_ID="${EDGE_WF_ID}" TRACE_ID="${TRACE_ID}" python3 - <<'PY'
import json, os, time, uuid
now = int(time.time() * 1000)
env = {
  "msg_id": "m_" + uuid.uuid4().hex,
  "ts_utc_ms": now,
  "type": "TASK_EVENT",
  "from": "node:node_trace_dummy",
  "to": "platform",
  "trace": {"trace_id": os.environ.get("TRACE_ID") or ""},
  "body": {
    "task_id": os.environ.get("EDGE_WF_ID") or "",
    "step_id": "s1",
    "state": "RUNNING",
    "idempotency_key": "k_trace_wf",
    "node_id": "node_trace_dummy",
  },
}
print(json.dumps(env))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/message")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${edge_msg}''')
if not obj.get("ok"):
  print("edge message failed", obj, file=sys.stderr)
  raise SystemExit(1)
PY

trace_q="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/trace?trace_id=$(TRACE_ID="${TRACE_ID}" python3 - <<'PY'
import os, urllib.parse
print(urllib.parse.quote(os.environ.get("TRACE_ID") or ""))
PY
)&limit=200&max_bytes=1048576")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${trace_q}''')
if not obj.get("ok"):
  print("trace query failed", obj, file=sys.stderr)
  raise SystemExit(1)
recs = obj.get("records") or []
if not isinstance(recs, list) or not recs:
  print("expected trace records", file=sys.stderr)
  print(obj, file=sys.stderr)
  raise SystemExit(1)

have_workflow_ev = any(isinstance(r, dict) and r.get("source") == "workflow_event" for r in recs)
have_edge_workflow_ev = any(isinstance(r, dict) and r.get("source") == "edge_workflow_event" for r in recs)

if not have_workflow_ev:
  print("missing workflow_event wrappers in trace response", file=sys.stderr)
  print(obj, file=sys.stderr)
  raise SystemExit(1)
if not have_edge_workflow_ev:
  print("missing edge_workflow_event wrappers in trace response", file=sys.stderr)
  print(obj, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_trace_workflow_events_smoke OK"
