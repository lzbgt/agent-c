#!/usr/bin/env bash
set -euo pipefail

AGENTD_BIN="${1:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "missing agentd binary path arg" >&2
  exit 2
fi

if [[ "${AGENT_DISABLE_NETWORK_TESTS:-}" == "1" ]]; then
  echo "SKIP: AGENT_DISABLE_NETWORK_TESTS=1" >&2
  exit 77
fi

# Always set proxy for network tests (can be required in some environments).
export https_proxy="${https_proxy:-${HTTPS_PROXY:-http://localhost:8120}}"
export http_proxy="${http_proxy:-${HTTP_PROXY:-http://localhost:8120}}"
export HTTPS_PROXY="${https_proxy}"
export HTTP_PROXY="${http_proxy}"
export no_proxy="${no_proxy:-${NO_PROXY:-127.0.0.1,localhost}}"
export NO_PROXY="${no_proxy}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

DEEPSEEK_KEY="${DEEPSEEK_API_KEY:-}"
PROJECT_MD="${PROJECT_ROOT}/project.md"
if [[ -z "${DEEPSEEK_KEY}" && -f "${PROJECT_MD}" ]]; then
  DEEPSEEK_KEY="$(grep -F -- "- deepseek:" "${PROJECT_MD}" | head -n 1 | sed -E 's/.*- deepseek:[[:space:]]*//')"
fi
if [[ -z "${DEEPSEEK_KEY}" ]]; then
  echo "SKIP: DEEPSEEK_API_KEY not set and not found in project.md" >&2
  exit 77
fi

BASE_URL="${DEEPSEEK_API_BASE:-https://api.deepseek.com}"
MODEL="${AGENT_TEST_DEEPSEEK_TOOL_MODEL:-deepseek-chat}"

PORT="$(( (RANDOM % 20000) + 20000 ))"
HOST="127.0.0.1"
DAEMON_URL="http://${HOST}:${PORT}"

SESSION_ID="agentd_cancel_$(date +%s)_$RANDOM"

cleanup() {
  if [[ -n "${AGENTD_PID:-}" ]]; then
    kill -TERM "${AGENTD_PID}" >/dev/null 2>&1 || true
    for _ in $(seq 1 30); do
      if ! kill -0 "${AGENTD_PID}" >/dev/null 2>&1; then
        break
      fi
      sleep 0.1
    done
    if kill -0 "${AGENTD_PID}" >/dev/null 2>&1; then
      kill -KILL "${AGENTD_PID}" >/dev/null 2>&1 || true
    fi
    wait "${AGENTD_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

LOG_DIR="${PROJECT_ROOT}/build"
mkdir -p "${LOG_DIR}"

DEEPSEEK_API_KEY="${DEEPSEEK_KEY}" "${AGENTD_BIN}" \
  --host "${HOST}" \
  --port "${PORT}" \
  --base-url "${BASE_URL}" \
  --model "${MODEL}" \
  --tools host \
  > "${LOG_DIR}/agentd_cancel_smoke.stdout.log" 2> "${LOG_DIR}/agentd_cancel_smoke.stderr.log" &
AGENTD_PID=$!

# Wait for health endpoint.
for _ in $(seq 1 60); do
  if curl -fsS --noproxy "*" --max-time 2 "${DAEMON_URL}/api/v1/health" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done
if ! curl -fsS --noproxy "*" --max-time 2 "${DAEMON_URL}/api/v1/health" >/dev/null 2>&1; then
  echo "agentd did not become healthy: ${DAEMON_URL}" >&2
  exit 1
fi

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
if status in ("done","error"):
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
if obj.get("status") not in ("done","error"):
  print("expected done/error status", obj, file=sys.stderr)
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
