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
NAME="agentd_workflow_list_query_smoke"

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}"

IDK="smoke_q_idk_${PORT_DAEMON}"
SESSION_ID="smoke_q_session_${PORT_DAEMON}"
TRACE_ID="smoke_q_trace_${PORT_DAEMON}"

submit_payload="$(cat <<JSON
{
  "session_id": "${SESSION_ID}",
  "trace_id": "${TRACE_ID}",
  "idempotency_key": "${IDK}",
  "tasks": [
    { "task_id": "D1", "kind": "delay", "delay_ms": 10 }
  ]
}
JSON
)"

submit_resp="$(curl -fsS -X POST "${DAEMON_URL}/api/v1/workflow/submit" \
  -H "Content-Type: application/json" \
  -d "${submit_payload}")"

workflow_id="$(python3 - <<'PY'
import json,sys
resp=json.load(sys.stdin)
if not resp.get("workflow_id"):
  print("", end="")
  sys.exit(1)
print(resp["workflow_id"], end="")
PY
<<<"${submit_resp}")"

if [[ -z "${workflow_id}" ]]; then
  echo "failed to get workflow_id from submit response" >&2
  echo "${submit_resp}" >&2
  exit 1
fi

check_query() {
  local label="$1"
  local query="$2"
  local resp
  resp="$(curl -fsS "${DAEMON_URL}/api/v1/workflows?status=all&limit=50&q=${query}")"
  python3 - <<'PY' "${label}" "${query}" "${workflow_id}" "${IDK}" "${SESSION_ID}" "${TRACE_ID}" <<<"${resp}"
import json,sys
label=sys.argv[1]
query=sys.argv[2]
workflow_id=sys.argv[3]
idk=sys.argv[4]
session_id=sys.argv[5]
trace_id=sys.argv[6]
resp=json.load(sys.stdin)
workflows=resp.get("workflows") or []
if not workflows:
  print(f"no workflows for query {label}={query}", file=sys.stderr)
  sys.exit(1)
for wf in workflows:
  if wf.get("workflow_id") == workflow_id:
    return
  if wf.get("idempotency_key") == idk and idk == query:
    return
  if wf.get("session_id") == session_id and session_id == query:
    return
  if wf.get("trace_id") == trace_id and trace_id == query:
    return
print(f"workflow not found for query {label}={query}", file=sys.stderr)
sys.exit(1)
PY
}

check_query "idempotency" "${IDK}"
check_query "session" "${SESSION_ID}"
check_query "trace" "${TRACE_ID}"

agentd_smoke_stop
exit 0
