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
  echo "SKIP: KIMI_API_KEY_CN (or MOONSHOT_API_KEY) not set and not found in .not_in_repo/project.local.md" >&2
  exit 77
fi

BASE_URL="${MOONSHOT_API_BASE:-https://api.moonshot.cn/v1}"
MODEL="${AGENT_TEST_MOONSHOT_MODEL:-kimi-k2.5}"

HOST="127.0.0.1"
PORT="$(agentd_smoke_pick_port)"

SESSION_ID="agentd_moonshot_tool_call_$(date +%s)_$RANDOM"

trap agentd_smoke_stop EXIT

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

KIMI_API_KEY_CN="${MOONSHOT_KEY}" agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "agentd_moonshot_tool_call" \
  --base-url "${BASE_URL}" \
  --model "${MODEL}" \
  --tools basic

agentd_smoke_wait_health "${DAEMON_URL}"

run_body="$(
  SESSION_ID="${SESSION_ID}" python3 - <<PY
import json, os
print(json.dumps({
  "prompt": "Use the calculator tool to compute (2+2)*10. Return exactly: 40",
  "session_id": os.environ["SESSION_ID"],
  "tools": "basic",
  "require_tool_call": True,
  "max_steps": 6,
  "max_chars": 20000,
  "keep_last": 16,
  "verbose": True
}))
PY
)"

run_resp="$(
  printf '%s' "${run_body}" | curl -fsS --noproxy "*" --max-time 240 \
    -H "Content-Type: application/json" \
    -d @- \
    "${DAEMON_URL}/api/v1/run"
)"

RUN_RESP="${run_resp}" python3 - <<PY
import json, os, sys
obj = json.loads(os.environ["RUN_RESP"])
if not obj.get("ok"):
  hs = obj.get("http_status")
  err = str(obj.get("error") or "")
  if hs == 429 and ("overloaded" in err.lower() or "rate" in err.lower()):
    print("SKIP: moonshot provider overloaded (429)", file=sys.stderr)
    raise SystemExit(77)
  print("daemon run failed", obj, file=sys.stderr)
  raise SystemExit(1)
events = obj.get("events") or []
if not isinstance(events, list):
  print("missing events array", file=sys.stderr)
  raise SystemExit(1)
has_tool_call = any(isinstance(e, dict) and e.get("type") == "tool_call" for e in events)
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
