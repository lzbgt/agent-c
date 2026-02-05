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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_workflow_stats_sessions_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

# Create a workflow that is associated with a session (allow_sessions=true).
# Use a deterministic delay task so no LLM/provider is needed.
submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<'PY'
import json
req = {
  "allow_sessions": True,
  "session_id": "sess_stats_1",
  "tasks": [
    {"task_id": "W", "kind": "delay", "delay_ms": 500}
  ]
}
print(json.dumps(req))
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

stats="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/workflow/stats?include_sessions=1&session_limit=16")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${stats}''')
if not obj.get("ok"):
  print("expected ok true", obj, file=sys.stderr)
  raise SystemExit(1)
rows = obj.get("sessions") or []
sid = "sess_stats_1"
got = None
for r in rows:
  if isinstance(r, dict) and r.get("session_id") == sid:
    got = r
    break
if not got:
  print("expected session row", sid, "in sessions:", rows, file=sys.stderr)
  raise SystemExit(1)
if int(got.get("inflight_tasks") or 0) < 1:
  print("expected inflight_tasks >= 1", got, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

echo "agentd_workflow_stats_sessions_smoke OK"

