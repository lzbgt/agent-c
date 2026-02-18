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
  echo "SKIP: DEEPSEEK_API_KEY not set and not found in .not_in_repo, project.local.md, or ~/.env" >&2
  exit 77
fi

BASE_URL="${DEEPSEEK_API_BASE:-https://api.deepseek.com}"
MODEL="${AGENT_TEST_DEEPSEEK_TOOL_MODEL:-deepseek-chat}"

HOST="127.0.0.1"
PORT="$(agentd_smoke_pick_port)"

SESSION_ID="agentd_host_tools_$(date +%s)_$RANDOM"

trap agentd_smoke_stop EXIT

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

DEEPSEEK_API_KEY="${DEEPSEEK_KEY}" agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "agentd_host_tools_smoke" \
  --base-url "${BASE_URL}" \
  --model "${MODEL}" \
  --tools host

agentd_smoke_wait_health "${DAEMON_URL}"

resp="$(curl -fsS \
  --noproxy "*" \
  --max-time 120 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "Use the shell_exec tool to run: echo OK. Then return exactly: OK",
  "session_id": "${SESSION_ID}",
  "tools": "host",
  "force_tool": "shell_exec",
  "require_tool_call": True,
  "max_steps": 4,
  "verbose": True,
  "yolo": True
}))
PY
)" \
  "${DAEMON_URL}/api/v1/run")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp}''')
if not obj.get("ok"):
  print("daemon run failed", obj, file=sys.stderr)
  raise SystemExit(1)
txt = (obj.get("assistant_text") or "").strip()
if txt != "OK":
  print(f"unexpected assistant_text: {txt!r}", file=sys.stderr)
  raise SystemExit(1)
events = obj.get("events") or []
if not any(e.get("type") == "tool_call" for e in events):
  print("expected tool_call event", file=sys.stderr)
  raise SystemExit(1)
PY

# Verify text_search tool is exposed + works end-to-end (model/tool loop/daemon event capture).
resp2="$(curl -fsS \
  --noproxy "*" \
  --max-time 120 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "Use the text_search tool with query=tool_text_search and path=${PROJECT_ROOT} (absolute). After the tool returns, return exactly: OK",
  "session_id": "${SESSION_ID}",
  "tools": "host",
  "force_tool": "text_search",
  "require_tool_call": True,
  "max_steps": 4,
  "verbose": True,
  "yolo": False
}))
PY
)" \
  "${DAEMON_URL}/api/v1/run")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp2}''')
if not obj.get("ok"):
  print("daemon run failed", obj, file=sys.stderr)
  raise SystemExit(1)
txt = (obj.get("assistant_text") or "").strip()
if txt != "OK":
  print(f"unexpected assistant_text: {txt!r}", file=sys.stderr)
  raise SystemExit(1)
events = obj.get("events") or []
tool_calls = [e for e in events if e.get("type") == "tool_call" and (e.get("data") or {}).get("tool_name") == "text_search"]
tool_results = [e for e in events if e.get("type") == "tool_result" and (e.get("data") or {}).get("tool_name") == "text_search"]
if not tool_calls:
  print("expected text_search tool_call event", file=sys.stderr)
  raise SystemExit(1)
if not tool_results:
  print("expected text_search tool_result event", file=sys.stderr)
  raise SystemExit(1)
content = (tool_results[-1].get("data") or {}).get("content") or ""
try:
  env = json.loads(content)
except Exception as e:
  print("failed to parse text_search tool_result content", e, file=sys.stderr)
  print(content[:500], file=sys.stderr)
  raise SystemExit(1)
if not env.get("ok"):
  print("text_search tool returned ok=false", env, file=sys.stderr)
  raise SystemExit(1)
data = env.get("data") or {}
if data.get("query") != "tool_text_search":
  print("unexpected query", data.get("query"), file=sys.stderr)
  raise SystemExit(1)
if data.get("path") != r'''${PROJECT_ROOT}''':
  print("unexpected path", data.get("path"), file=sys.stderr)
  raise SystemExit(1)
matches = data.get("matches") or []
if not isinstance(matches, list) or len(matches) < 1:
  print("expected at least one match", file=sys.stderr)
  raise SystemExit(1)
if not any((m.get("path") or "").endswith("cli/src/toolset_host.cpp") for m in matches if isinstance(m, dict)):
  print("expected match path to include cli/src/toolset_host.cpp", file=sys.stderr)
  raise SystemExit(1)
PY

# Cleanup session artifacts.
curl -fsS --noproxy "*" -X DELETE "${DAEMON_URL}/api/v1/session?session_id=$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote("${SESSION_ID}"))
PY
)" --max-time 10 >/dev/null || true
