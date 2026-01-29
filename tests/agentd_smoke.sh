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
# Do not proxy localhost daemon traffic.
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

SESSION_ID="agentd_smoke_$(date +%s)_$RANDOM"

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
  --tools basic \
  > "${LOG_DIR}/agentd_smoke.stdout.log" 2> "${LOG_DIR}/agentd_smoke.stderr.log" &
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

post_run() {
  local body="$1"
  curl -fsS \
    --noproxy "*" \
    --max-time 90 \
    -H "Content-Type: application/json" \
    -d "${body}" \
    "${DAEMON_URL}/api/v1/run"
}

# Warm up a session with a few medium messages so the next run can trigger compaction.
for i in 1 2 3; do
  post_run "$(python3 - <<PY
import json
prompt = ("warmup-%d: " % ${i}) + ("x" * 600)
print(json.dumps({"prompt": prompt, "session_id": "${SESSION_ID}", "tools": "none", "max_chars": 20000, "keep_last": 16}))
PY
)" >/dev/null
done

# Now force a very small budget and require a tool call so we get a tool-loop + compaction events.
resp="$(post_run "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "Use the calculator tool to compute (2+2)*10. Return exactly: 40",
  "session_id": "${SESSION_ID}",
  "tools": "basic",
  "force_tool": "calculator",
  "require_tool_call": True,
  "max_steps": 6,
  "max_chars": 500,
  "keep_last": 4,
  "verbose": True
}))
PY
)")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp}''')
if not obj.get("ok"):
  print("daemon run failed", obj, file=sys.stderr)
  raise SystemExit(1)
events = obj.get("events") or []
if not isinstance(events, list):
  print("missing events array", file=sys.stderr)
  raise SystemExit(1)
has_compaction = any(e.get("type") == "compaction" and isinstance(e.get("data"), dict) and e["data"].get("dropped_messages", 0) > 0 for e in events)
has_tool_call = any(e.get("type") == "tool_call" for e in events)
if not has_compaction:
  print("expected compaction event with dropped_messages>0", file=sys.stderr)
  raise SystemExit(1)
if not has_tool_call:
  print("expected tool_call event", file=sys.stderr)
  raise SystemExit(1)
txt = (obj.get("assistant_text") or "").strip()
if txt != "40":
  print(f"unexpected assistant_text: {txt!r}", file=sys.stderr)
  raise SystemExit(1)
PY

# Cleanup session artifacts.
curl -fsS --noproxy "*" -X DELETE "${DAEMON_URL}/api/v1/session?session_id=$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote("${SESSION_ID}"))
PY
)" --max-time 10 >/dev/null || true
