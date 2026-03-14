#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "usage: agentd_workflow_schedule_smoke.sh <agentd_bin>" >&2
  exit 2
fi

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

PORT_DAEMON="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"
DB_PATH="${LOG_DIR}/agentd_workflow_schedule_smoke_${PORT_DAEMON}.sqlite"
STATE_DIR="${LOG_DIR}/agentd_workflow_schedule_smoke_${PORT_DAEMON}.state"
SCHEDULE_ID="sched-smoke-${PORT_DAEMON}"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_workflow_schedule_smoke" \
  --db-path "${DB_PATH}" \
  --state-dir "${STATE_DIR}" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

create_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "schedule_id": "${SCHEDULE_ID}",
  "cron": "0 0 1 1 *",
  "timezone": "UTC",
  "spec": {
    "tasks": [
      {
        "id": "task-1",
        "kind": "llm",
        "prompt": "hello from schedule smoke"
      }
    ]
  }
}))
PY
)" \
  "${DAEMON_URL}/api/v1/workflow_schedules")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${create_resp}''')
if not obj.get("ok"):
  print("schedule create failed", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("schedule_id") != "${SCHEDULE_ID}":
  print("unexpected schedule_id", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("status") != "active":
  print("unexpected create status", obj, file=sys.stderr)
  raise SystemExit(1)
if not isinstance(obj.get("next_tick_unix_ms"), int) or obj.get("next_tick_unix_ms", 0) <= 0:
  print("missing next_tick_unix_ms", obj, file=sys.stderr)
  raise SystemExit(1)
PY

list_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/workflow_schedules?status=active&limit=20&offset=0")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${list_resp}''')
schedules = obj.get("schedules") or []
match = next((s for s in schedules if s.get("schedule_id") == "${SCHEDULE_ID}"), None)
if not match:
  print("schedule missing from list", obj, file=sys.stderr)
  raise SystemExit(1)
if match.get("status") != "active":
  print("unexpected list status", match, file=sys.stderr)
  raise SystemExit(1)
if match.get("cron") != "0 0 1 1 *":
  print("unexpected cron", match, file=sys.stderr)
  raise SystemExit(1)
PY

get_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/workflow_schedule?schedule_id=${SCHEDULE_ID}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${get_resp}''')
schedule = obj.get("schedule") or {}
if not obj.get("ok"):
  print("schedule get failed", obj, file=sys.stderr)
  raise SystemExit(1)
if schedule.get("schedule_id") != "${SCHEDULE_ID}" or schedule.get("status") != "active":
  print("unexpected schedule body", schedule, file=sys.stderr)
  raise SystemExit(1)
if schedule.get("timezone") != "UTC":
  print("unexpected timezone", schedule, file=sys.stderr)
  raise SystemExit(1)
PY

runs_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/workflow_schedule/runs?schedule_id=${SCHEDULE_ID}&limit=10&offset=0")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${runs_resp}''')
if not obj.get("ok"):
  print("schedule runs failed", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("schedule_id") != "${SCHEDULE_ID}":
  print("unexpected runs schedule_id", obj, file=sys.stderr)
  raise SystemExit(1)
runs = obj.get("runs")
if not isinstance(runs, list):
  print("runs is not a list", obj, file=sys.stderr)
  raise SystemExit(1)
PY

pause_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "{\"schedule_id\":\"${SCHEDULE_ID}\"}" \
  "${DAEMON_URL}/api/v1/workflow_schedule/pause")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${pause_resp}''')
if not obj.get("ok") or obj.get("status") != "paused":
  print("schedule pause failed", obj, file=sys.stderr)
  raise SystemExit(1)
PY

paused_get_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/workflow_schedule?schedule_id=${SCHEDULE_ID}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${paused_get_resp}''')
schedule = obj.get("schedule") or {}
if schedule.get("status") != "paused":
  print("schedule did not pause", schedule, file=sys.stderr)
  raise SystemExit(1)
PY

resume_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "{\"schedule_id\":\"${SCHEDULE_ID}\"}" \
  "${DAEMON_URL}/api/v1/workflow_schedule/resume")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resume_resp}''')
if not obj.get("ok") or obj.get("status") != "active":
  print("schedule resume failed", obj, file=sys.stderr)
  raise SystemExit(1)
if not isinstance(obj.get("next_tick_unix_ms"), int) or obj.get("next_tick_unix_ms", 0) <= 0:
  print("resume missing next_tick_unix_ms", obj, file=sys.stderr)
  raise SystemExit(1)
PY

invalid_cron_status="$(curl -sS --noproxy "*" -o "${LOG_DIR}/agentd_workflow_schedule_invalid_cron_${PORT_DAEMON}.json" \
  -w "%{http_code}" --max-time 10 \
  -H "Content-Type: application/json" \
  -d '{"schedule_id":"bad-cron","cron":"bad cron","timezone":"UTC","spec":{"tasks":[{"id":"task-1","kind":"llm","prompt":"oops"}]}}' \
  "${DAEMON_URL}/api/v1/workflow_schedules")"
if [[ "${invalid_cron_status}" != "400" ]]; then
  echo "expected invalid cron create to return 400, got ${invalid_cron_status}" >&2
  cat "${LOG_DIR}/agentd_workflow_schedule_invalid_cron_${PORT_DAEMON}.json" >&2
  exit 1
fi

delete_resp="$(curl -fsS --noproxy "*" --max-time 10 -X DELETE \
  "${DAEMON_URL}/api/v1/workflow_schedule?schedule_id=${SCHEDULE_ID}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${delete_resp}''')
if not obj.get("ok") or obj.get("schedule_id") != "${SCHEDULE_ID}":
  print("schedule delete failed", obj, file=sys.stderr)
  raise SystemExit(1)
PY

delete_get_status="$(curl -sS --noproxy "*" -o "${LOG_DIR}/agentd_workflow_schedule_deleted_${PORT_DAEMON}.json" \
  -w "%{http_code}" --max-time 10 \
  "${DAEMON_URL}/api/v1/workflow_schedule?schedule_id=${SCHEDULE_ID}")"
if [[ "${delete_get_status}" != "404" ]]; then
  echo "expected deleted schedule get to return 404, got ${delete_get_status}" >&2
  cat "${LOG_DIR}/agentd_workflow_schedule_deleted_${PORT_DAEMON}.json" >&2
  exit 1
fi
