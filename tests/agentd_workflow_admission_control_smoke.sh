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
NAME="agentd_workflow_admission_control_smoke"

DB_PATH="${LOG_DIR}/${NAME}_${PORT_DAEMON}.sqlite"
STATE_DIR="${LOG_DIR}/${NAME}_${PORT_DAEMON}.state"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}" \
  --db-path "${DB_PATH}" \
  --state-dir "${STATE_DIR}" \
  --tools none \
  --workflow-admit-max-inflight-tasks-per-session 1

agentd_smoke_wait_health "${DAEMON_URL}"

submit1="$(curl -sS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<'PY'
import json
req = {
  "allow_sessions": True,
  "session_id": "sess_admit_1",
  "tasks": [
    {"task_id": "A", "kind": "delay", "delay_ms": 1500, "result": {"assistant_text": "A"}}
  ],
}
print(json.dumps(req))
PY
)" \
  -w "\nHTTP_STATUS:%{http_code}\n" \
  "${DAEMON_URL}/api/v1/workflow/submit")"

wf1="$(python3 - <<PY
import json, sys
s = r'''${submit1}'''
marker = "HTTP_STATUS:"
if marker not in s:
  print("missing status marker in response", s, file=sys.stderr)
  raise SystemExit(1)
body, status = s.rsplit(marker, 1)
status = int(status.strip() or "0")
if status != 200:
  print("submit1 failed status", status, "body:", body, file=sys.stderr)
  raise SystemExit(1)
obj = json.loads(body)
print(obj.get("workflow_id",""))
PY
)"
if [[ -z "${wf1}" ]]; then
  echo "missing workflow_id in submit1: ${submit1}" >&2
  exit 1
fi

# Second submit should be rejected (429) because the first task is still queued|running for the same session.
submit2="$(curl -sS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<'PY'
import json
req = {
  "allow_sessions": True,
  "session_id": "sess_admit_1",
  "tasks": [
    {"task_id": "B", "kind": "delay", "delay_ms": 0, "result": {"assistant_text": "B"}}
  ],
}
print(json.dumps(req))
PY
)" \
  -w "\nHTTP_STATUS:%{http_code}\n" \
  "${DAEMON_URL}/api/v1/workflow/submit")"

python3 - <<PY
import json, sys
s = r'''${submit2}'''
marker = "HTTP_STATUS:"
if marker not in s:
  print("missing status marker in response", s, file=sys.stderr)
  raise SystemExit(1)
body, status = s.rsplit(marker, 1)
status = int(status.strip() or "0")
if status != 429:
  print("expected HTTP 429, got", status, "body:", body, file=sys.stderr)
  raise SystemExit(1)
obj = json.loads(body)
if obj.get("ok") is not False:
  print("expected ok=false", obj, file=sys.stderr)
  raise SystemExit(1)
if "admission control" not in (obj.get("error","").lower()):
  print("expected admission control error", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("limit_inflight_tasks_per_session") != 1:
  print("expected limit_inflight_tasks_per_session=1", obj, file=sys.stderr)
  raise SystemExit(1)
if not isinstance(obj.get("retry_after_ms"), int):
  print("expected retry_after_ms int", obj, file=sys.stderr)
  raise SystemExit(1)
PY

# Wait for workflow 1 to complete, then a third submit should be accepted.
final=""
for _ in $(seq 1 200); do
  final="$(curl -fsS --noproxy "*" --max-time 5 \
    "${DAEMON_URL}/api/v1/workflow?workflow_id=${wf1}&include_tasks=0&include_results=0")"
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

submit3="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<'PY'
import json
req = {
  "allow_sessions": True,
  "session_id": "sess_admit_1",
  "tasks": [
    {"task_id": "C", "kind": "delay", "delay_ms": 0, "result": {"assistant_text": "C"}}
  ],
}
print(json.dumps(req))
PY
)" \
  "${DAEMON_URL}/api/v1/workflow/submit")"

wf3="$(python3 - <<PY
import json
obj = json.loads(r'''${submit3}''')
print(obj.get("workflow_id",""))
PY
)"
if [[ -z "${wf3}" ]]; then
  echo "missing workflow_id in submit3: ${submit3}" >&2
  exit 1
fi

echo "${NAME} OK"
