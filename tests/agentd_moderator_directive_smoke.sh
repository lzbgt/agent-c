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

PORT_DAEMON="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"
SESSION_ID="agentd_moderator_smoke_$(date +%s)_$RANDOM"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_moderator_directive_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

resp_dir="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${SESSION_ID}",
  "directive": "pause automation until next update",
  "scope": "all",
  "assignees": ["agent-a", "role:planner"],
  "actor": {"id": "moderator-smoke", "kind": "test"}
}))
PY
)" \
  "${DAEMON_URL}/api/v1/moderator/directive")"

resp_task="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${SESSION_ID}",
  "title": "collect system status",
  "detail": "run diagnostics and summarize",
  "assignees": ["agent-b", "role:executor"],
  "actor": {"id": "moderator-smoke", "kind": "test"}
}))
PY
)" \
  "${DAEMON_URL}/api/v1/moderator/task")"

resp_events="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/moderator/events?session_id=${SESSION_ID}")"

RESP_DIR="${resp_dir}" RESP_TASK="${resp_task}" RESP_EVENTS="${resp_events}" python3 - <<'PY'
import json, os, sys

dir_obj = json.loads(os.environ["RESP_DIR"])
if not dir_obj.get("ok"):
  print("directive not ok:", dir_obj, file=sys.stderr)
  raise SystemExit(1)
if dir_obj.get("type") != "moderator_directive":
  print("unexpected directive type:", dir_obj.get("type"), file=sys.stderr)
  raise SystemExit(1)
event = dir_obj.get("event") or {}
data = event.get("data") or {}
if data.get("scope") != "all":
  print("missing directive scope:", data, file=sys.stderr)
  raise SystemExit(1)
assignees = data.get("assignees") or []
if "agent-a" not in assignees:
  print("missing directive assignee:", assignees, file=sys.stderr)
  raise SystemExit(1)

task_obj = json.loads(os.environ["RESP_TASK"])
if not task_obj.get("ok"):
  print("task not ok:", task_obj, file=sys.stderr)
  raise SystemExit(1)
if task_obj.get("type") != "moderator_task_published":
  print("unexpected task type:", task_obj.get("type"), file=sys.stderr)
  raise SystemExit(1)
task_event = task_obj.get("event") or {}
task_data = task_event.get("data") or {}
task = task_data.get("task") or {}
task_assignees = task.get("assignees") or []
if "agent-b" not in task_assignees:
  print("missing task assignee:", task_assignees, file=sys.stderr)
  raise SystemExit(1)

ev_obj = json.loads(os.environ["RESP_EVENTS"])
if not ev_obj.get("ok"):
  print("events not ok:", ev_obj, file=sys.stderr)
  raise SystemExit(1)

events = ev_obj.get("events") or []
types = {e.get("type") for e in events if isinstance(e, dict)}
missing = {"moderator_directive", "moderator_task_published"} - types
if missing:
  print("missing moderator event types:", sorted(missing), file=sys.stderr)
  raise SystemExit(1)
print("agentd_moderator_directive_smoke OK")
PY
