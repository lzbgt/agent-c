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

PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

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

SESSION_ID="agentd_smoke_$(date +%s)_$RANDOM"

trap agentd_smoke_stop EXIT

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

DEEPSEEK_API_KEY="${DEEPSEEK_KEY}" agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "agentd_smoke" \
  --base-url "${BASE_URL}" \
  --model "${MODEL}" \
  --tools basic

agentd_smoke_wait_health "${DAEMON_URL}"

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
