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
# This test requires OpenAI tool calling (tool_choice). Use a tools-capable DeepSeek model by default.
MODEL="${AGENT_TEST_DEEPSEEK_MODEL:-deepseek-chat}"

HOST="127.0.0.1"
PORT="$(agentd_smoke_pick_port)"

trap agentd_smoke_stop EXIT

DEEPSEEK_API_KEY="${DEEPSEEK_KEY}" agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "agentd_async_smoke" \
  --base-url "${BASE_URL}" \
  --model "${MODEL}" \
  --tools basic

agentd_smoke_wait_health "${DAEMON_URL}"

job_json="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "Use the calculator tool to compute (2+2)*10. Return exactly: 40",
  "session_id": "agentd_async_smoke",
  "no_session": True,
  "tools": "basic",
  "force_tool": "calculator",
  "require_tool_call": True,
  "max_steps": 6,
  "trace": False
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
found_start=0
cursor=0
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
  set +e
  python3 - <<PY
import json, sys
obj=json.loads(r'''${st}''')
if not obj.get("ok"):
  print("job query failed", obj, file=sys.stderr)
  raise SystemExit(1)
events=obj.get("events") or []
if isinstance(events, list) and events:
  # If we can observe events before completion, the UI can show progress (no more "hang").
  # Expect a 'start' event for tool loops.
  if any(e.get("type") == "start" for e in events):
    raise SystemExit(3)
status=obj.get("status")
if status in ("done","error"):
  res=obj.get("result")
  if not isinstance(res, dict):
    print("missing result", obj, file=sys.stderr)
    raise SystemExit(1)
  if not res.get("ok"):
    print("run failed", res, file=sys.stderr)
    raise SystemExit(1)
  txt=(res.get("assistant_text") or "").strip()
  if txt != "40":
    print(f"unexpected assistant_text: {txt!r}", file=sys.stderr)
    raise SystemExit(1)
  raise SystemExit(0)
raise SystemExit(2)
PY
  rc=$?
  set -e
  if [[ $rc -eq 0 ]]; then
    break
  elif [[ $rc -eq 3 ]]; then
    found_start=1
  elif [[ $rc -eq 1 ]]; then
    exit 1
  fi
  # Update cursor for the next poll.
  cursor="$(python3 - <<PY
import json
obj=json.loads(r'''${st}''')
print(int(obj.get("events_cursor_next") or 0))
PY
)"
  sleep 0.2
done

if [[ "${found_start}" -ne 1 ]]; then
  echo "did not observe start event while job was running; live progress may be broken" >&2
  exit 1
fi

# Best-effort cleanup.
curl -fsS --noproxy "*" -X DELETE "${DAEMON_URL}/api/v1/job?job_id=$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote('${job_id}'))
PY
)" --max-time 5 >/dev/null || true
