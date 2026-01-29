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

SESSION_ID="agentd_host_tools_$(date +%s)_$RANDOM"

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
  --host-scope "${PROJECT_ROOT}" \
  > "${LOG_DIR}/agentd_host_tools_smoke.stdout.log" 2> "${LOG_DIR}/agentd_host_tools_smoke.stderr.log" &
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
  "tools_root": "@host",
  "force_tool": "shell_exec",
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
  "prompt": "Use the text_search tool with query=tool_text_search and path=. (repo root). After the tool returns, return exactly: OK",
  "session_id": "${SESSION_ID}",
  "tools": "host",
  "tools_root": "@host",
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
if data.get("path") != ".":
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
