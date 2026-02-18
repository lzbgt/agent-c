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

if [[ "${AGENT_DISABLE_NETWORK_TESTS:-}" == "1" ]]; then
  echo "SKIP: AGENT_DISABLE_NETWORK_TESTS=1" >&2
  exit 77
fi

source "${SCRIPT_DIR}/test_keys.sh"
agent_test_setup_proxy_env
DEEPSEEK_KEY="$(agent_test_get_key deepseek 2>/dev/null || true)"
if [[ -z "${DEEPSEEK_KEY}" ]]; then
  echo "SKIP: DEEPSEEK_API_KEY not set and not found in .not_in_repo, project.local.md, or ~/.env" >&2
  exit 77
fi

BASE_URL="${DEEPSEEK_API_BASE:-https://api.deepseek.com}"
MODEL="${AGENT_TEST_DEEPSEEK_TOOL_MODEL:-deepseek-chat}"

HOST="127.0.0.1"
PORT="$(agentd_smoke_pick_port)"

SESSION_ID="agentd_heartbeat_$(date +%s)_$RANDOM"

trap agentd_smoke_stop EXIT

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

DEEPSEEK_API_KEY="${DEEPSEEK_KEY}" agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "agentd_heartbeat_smoke" \
  --base-url "${BASE_URL}" \
  --model "${MODEL}" \
  --tools host

agentd_smoke_wait_health "${DAEMON_URL}"

job_json="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "Use the shell_exec tool to run exactly: sleep 3; echo OK. Then return exactly: OK",
  "session_id": "${SESSION_ID}",
  "no_session": True,
  "tools": "host",
  "force_tool": "shell_exec",
  "require_tool_call": True,
  "max_steps": 3,
  "trace": False,
  "verbose": False,
  "yolo": True
}))
PY
)" \
  "${DAEMON_URL}/api/v1/run_async")"

job_id="$(python3 - <<PY
import json, sys
obj=json.loads(r'''${job_json}''')
if not obj.get("ok") or not obj.get("job_id"):
  print("bad run_async response", obj, file=sys.stderr)
  raise SystemExit(1)
print(obj["job_id"])
PY
)"

deadline=$(( $(date +%s) + 60 ))
cursor=0
saw_tool_call=0
saw_tool_result=0
heartbeat_after_tool_call=0

while true; do
  now=$(date +%s)
  if [[ "${now}" -gt "${deadline}" ]]; then
    echo "timed out waiting for job ${job_id}" >&2
    exit 1
  fi

  st="$(curl -fsS --noproxy "*" --max-time 10 "${DAEMON_URL}/api/v1/job?job_id=$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote('${job_id}'))
PY
)&include_events=1&cursor=${cursor}&max_events=256")"

  # Scan events for tool_call/tool_result/heartbeat.
  scan="$(python3 - <<PY
import json
obj=json.loads(r'''${st}''')
events=obj.get("events") or []
flags={"tool_call":0,"tool_result":0,"heartbeat":0,"status":obj.get("status") or "","next":int(obj.get("events_cursor_next") or 0)}
for e in events:
  t=e.get("type")
  if t=="tool_call":
    d=e.get("data") or {}
    if d.get("tool_name")=="shell_exec":
      flags["tool_call"]=1
  if t=="tool_result":
    d=e.get("data") or {}
    if d.get("tool_name")=="shell_exec":
      flags["tool_result"]=1
  if t=="heartbeat":
    flags["heartbeat"]=1
print(flags["tool_call"], flags["tool_result"], flags["heartbeat"], flags["status"], flags["next"])
PY
)"

  tc="$(echo "${scan}" | awk '{print $1}')"
  tr="$(echo "${scan}" | awk '{print $2}')"
  hb="$(echo "${scan}" | awk '{print $3}')"
  status="$(echo "${scan}" | awk '{print $4}')"
  next="$(echo "${scan}" | awk '{print $5}')"

  if [[ "${tc}" == "1" ]]; then
    saw_tool_call=1
  fi
  if [[ "${hb}" == "1" && "${saw_tool_call}" == "1" ]]; then
    heartbeat_after_tool_call=1
  fi
  if [[ "${tr}" == "1" ]]; then
    saw_tool_result=1
  fi

  if [[ "${status}" == "done" || "${status}" == "error" ]]; then
    break
  fi

  cursor="${next}"
  sleep 0.2
done

final="$(curl -fsS --noproxy "*" --max-time 10 "${DAEMON_URL}/api/v1/job?job_id=$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote('${job_id}'))
PY
)")"

python3 - <<PY
import json, sys
obj=json.loads(r'''${final}''')
if not obj.get("ok"):
  print("job query failed", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("status") not in ("done","error"):
  print("expected done/error status", obj, file=sys.stderr)
  raise SystemExit(1)
PY

if [[ "${saw_tool_call}" -ne 1 ]]; then
  echo "expected to see shell_exec tool_call" >&2
  exit 1
fi
if [[ "${saw_tool_result}" -ne 1 ]]; then
  echo "expected to see shell_exec tool_result" >&2
  exit 1
fi
if [[ "${heartbeat_after_tool_call}" -ne 1 ]]; then
  echo "expected to see a heartbeat event after tool_call" >&2
  exit 1
fi
