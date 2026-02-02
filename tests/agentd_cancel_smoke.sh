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
  echo "SKIP: DEEPSEEK_API_KEY not set and not found in project.local.md" >&2
  exit 77
fi

BASE_URL="${DEEPSEEK_API_BASE:-https://api.deepseek.com}"
MODEL="${AGENT_TEST_DEEPSEEK_TOOL_MODEL:-deepseek-chat}"

HOST="127.0.0.1"
PORT="$(agentd_smoke_pick_port)"

SESSION_ID="agentd_cancel_$(date +%s)_$RANDOM"

trap agentd_smoke_stop EXIT

DEEPSEEK_API_KEY="${DEEPSEEK_KEY}" agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "agentd_cancel_smoke" \
  --base-url "${BASE_URL}" \
  --model "${MODEL}" \
  --tools host

agentd_smoke_wait_health "${DAEMON_URL}"

job_json="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "Use the shell_exec tool to run exactly: sleep 20; echo OK. Then return exactly: OK",
  "session_id": "${SESSION_ID}",
  "no_session": True,
  "tools": "host",
  "force_tool": "shell_exec",
  "require_tool_call": True,
  "max_steps": 4,
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

deadline=$(( $(date +%s) + 120 ))
cursor=0
cancelled=0
cancel_time=0

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

  # If we saw a tool_call, request cancellation (best-effort) and then wait for completion.
  if [[ "${cancelled}" -eq 0 ]]; then
    set +e
    python3 - <<PY
import json, sys
obj=json.loads(r'''${st}''')
events=obj.get("events") or []
for e in events:
  if e.get("type") == "tool_call":
    d=e.get("data") or {}
    if d.get("tool_name") == "shell_exec":
      raise SystemExit(3)
raise SystemExit(0)
PY
    rc=$?
    set -e
    if [[ $rc -eq 3 ]]; then
      curl -fsS --noproxy "*" --max-time 5 -X POST "${DAEMON_URL}/api/v1/job/cancel?job_id=$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote('${job_id}'))
PY
)" >/dev/null
      cancelled=1
      cancel_time=$(date +%s)
    fi
  fi

  set +e
  python3 - <<PY
import json, sys
obj=json.loads(r'''${st}''')
if not obj.get("ok"):
  print("job query failed", obj, file=sys.stderr)
  raise SystemExit(1)
status=obj.get("status")
if status in ("done","error","cancelled"):
  raise SystemExit(3)
raise SystemExit(0)
PY
  rc=$?
  set -e
  if [[ $rc -eq 3 ]]; then
    break
  fi

  cursor="$(python3 - <<PY
import json
obj=json.loads(r'''${st}''')
print(int(obj.get("events_cursor_next") or 0))
PY
)"
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
if obj.get("status") not in ("done","error","cancelled"):
  print("expected done/error/cancelled status", obj, file=sys.stderr)
  raise SystemExit(1)
res=obj.get("result") or {}
if res.get("ok") is True:
  print("expected cancelled job to not be ok", res, file=sys.stderr)
  raise SystemExit(1)
err=(res.get("error") or "") + " " + (obj.get("error") or "")
if "cancel" not in err.lower():
  print("expected cancel in error", err, file=sys.stderr)
  raise SystemExit(1)
PY

if [[ "${cancelled}" -ne 1 ]]; then
  echo "did not manage to send cancel request" >&2
  exit 1
fi

# Ensure cancellation finishes quickly (should not wait 20s).
end_time=$(date +%s)
dt=$(( end_time - cancel_time ))
if [[ "${dt}" -gt 10 ]]; then
  echo "cancel took too long: ${dt}s" >&2
  exit 1
fi

# Best-effort cleanup.
curl -fsS --noproxy "*" -X DELETE "${DAEMON_URL}/api/v1/job?job_id=$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote('${job_id}'))
PY
)" --max-time 5 >/dev/null || true
