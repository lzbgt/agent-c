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
MOONSHOT_KEY="$(agent_test_get_key moonshot 2>/dev/null || true)"
if [[ -z "${MOONSHOT_KEY}" ]]; then
  echo "SKIP: KIMI_API_KEY_CN (or MOONSHOT_API_KEY) not set and not found in .not_in_repo, project.local.md, or ~/.env" >&2
  exit 77
fi

BASE_URL="${MOONSHOT_API_BASE:-https://api.moonshot.cn/v1}"
MODEL_PRIMARY="${AGENT_TEST_MOONSHOT_STREAM_TOOL_MODEL:-${AGENT_TEST_MOONSHOT_TOOL_MODEL:-${AGENT_TEST_MOONSHOT_MODEL:-kimi-k2.5}}}"

HOST="127.0.0.1"
PORT="$(agentd_smoke_pick_port)"

trap agentd_smoke_stop EXIT

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

KIMI_API_KEY_CN="${MOONSHOT_KEY}" agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "agentd_moonshot_stream_tool_call_smoke" \
  --base-url "${BASE_URL}" \
  --model "${MODEL_PRIMARY}" \
  --tools basic

agentd_smoke_wait_health "${DAEMON_URL}"

session_id="agentd_stream_moonshot_tool_$(date +%s)_$RANDOM"

job="$(curl -fsS \
  --noproxy "*" \
  --max-time 30 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "Use the calculator tool to compute 7*6. After the tool result, respond with exactly: 42",
  "session_id": "${session_id}",
  "tools": "basic",
  "stream_assistant": True,
  "model": "${MODEL_PRIMARY}",
  "force_tool": "calculator",
  "require_tool_call": True,
  "max_chars": 20000,
  "keep_last": 16,
  "verbose": False,
  "trace": False
}))
PY
)" \
  "${DAEMON_URL}/api/v1/run_async")"

job_id="$(python3 - <<PY
import json
obj = json.loads(r'''${job}''')
print(obj.get("job_id",""))
PY
)"
if [[ -z "${job_id}" ]]; then
  echo "missing job_id: ${job}" >&2
  exit 1
fi

job_id_q="$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${job_id}")"
stream="$(curl -fsS --noproxy "*" --max-time 120 "${DAEMON_URL}/api/v1/job/stream?job_id=${job_id_q}&cursor=0" | head -n 8000)"

STREAM="${stream}" python3 - <<PY
import re, sys, os
s = os.environ["STREAM"]
lower = s.lower()
if ('"http_status":429' in s or '"status":429' in s) and ("overloaded" in lower or "rate" in lower):
  print("SKIP: moonshot provider overloaded (429)", file=sys.stderr)
  raise SystemExit(77)
if ('"type":"error"' in s or '"type": "error"' in s):
  if ("unsupported" in lower or "not support" in lower or "not_supported" in lower) and ("tool" in lower or "stream" in lower):
    print("SKIP: moonshot provider does not support tool-call streaming", file=sys.stderr)
    raise SystemExit(77)
if 'event: job_done' not in s:
  print("missing job_done in SSE stream", file=sys.stderr)
  raise SystemExit(1)
has_tool_call = False
for m in re.finditer(r"^data: (.*)$", s, flags=re.M):
  line = m.group(1)
  if '"type":"tool_call"' in line or '"type": "tool_call"' in line:
    has_tool_call = True
if not has_tool_call:
  print("expected at least one tool_call event in stream", file=sys.stderr)
  raise SystemExit(1)
PY

# Cleanup session artifacts.
curl -fsS --noproxy "*" -X DELETE "${DAEMON_URL}/api/v1/session?session_id=$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote("${session_id}"))
PY
)" --max-time 10 >/dev/null || true
