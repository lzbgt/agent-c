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

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

PORT_DAEMON="$(agentd_smoke_pick_port)"
PORT_STUB="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"
STUB_BASE="http://${HOST}:${PORT_STUB}/v1"
NAME="agentd_approval_rules_smoke"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

# Stub provider:
# - First response: tool call to calculator
# - Second response: assistant "OK"
python3 -u - <<PY > "${LOG_DIR}/${NAME}.stub.stdout.log" 2> "${LOG_DIR}/${NAME}.stub.stderr.log" &
import json
from http.server import BaseHTTPRequestHandler, HTTPServer

def has_tool_result(messages):
  for m in messages:
    if isinstance(m, dict) and m.get("role") == "tool":
      return True
  return False

class H(BaseHTTPRequestHandler):
  def log_message(self, fmt, *args):
    return

  def do_POST(self):
    if self.path != "/v1/chat/completions":
      self.send_response(404)
      self.end_headers()
      return
    length = int(self.headers.get("content-length") or "0")
    raw = self.rfile.read(length) if length > 0 else b"{}"
    try:
      req = json.loads(raw.decode("utf-8"))
    except Exception:
      req = {}
    messages = req.get("messages") if isinstance(req.get("messages"), list) else []

    if has_tool_result(messages):
      body = {
        "id": "cmpl_stub_2",
        "object": "chat.completion",
        "created": 0,
        "model": "stub",
        "choices": [
          {"index": 0, "message": {"role": "assistant", "content": "OK"}, "finish_reason": "stop"}
        ],
      }
    else:
      body = {
        "id": "cmpl_stub_1",
        "object": "chat.completion",
        "created": 0,
        "model": "stub",
        "choices": [
          {
            "index": 0,
            "message": {
              "role": "assistant",
              "content": "",
              "tool_calls": [
                {
                  "id": "call_1",
                  "type": "function",
                  "function": {
                    "name": "calculator",
                    "arguments": json.dumps({"expression": "2+2"}),
                  },
                }
              ],
            },
            "finish_reason": "tool_calls",
          }
        ],
      }

    data = json.dumps(body).encode("utf-8")
    self.send_response(200)
    self.send_header("Content-Type", "application/json; charset=utf-8")
    self.send_header("Content-Length", str(len(data)))
    self.end_headers()
    self.wfile.write(data)

HTTPServer(("127.0.0.1", ${PORT_STUB}), H).serve_forever()
PY
STUB_PID=$!

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}" \
  --tools basic

agentd_smoke_wait_health "${DAEMON_URL}"

TRACE_ID="trace_${NAME}_${PORT_DAEMON}"

job_json="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "Use calculator for 2+2 and reply OK",
  "no_session": True,
  "tools": "basic",
  "base_url": "${STUB_BASE}",
  "api_key": "dummy",
  "model": "stub",
  "max_steps": 4,
  "trace": False,
  "trace_id": "${TRACE_ID}",
  "policy_mode": "enforce",
  "policy_approval_poll_ms": 100,
  "policy_approval_rules": [
    {
      "tool_names": ["calculator"],
      "min_approvals": 2,
      "role_allowlist": ["planner", "reviewer"],
      "require_distinct_roles": True,
      "timeout_ms": 60000,
      "quorum_mode": "strict"
    }
  ]
}))
PY
)" \
  "${DAEMON_URL}/api/v1/run_async")"

job_id="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${job_json}''')
if not obj.get("ok") or not obj.get("job_id"):
  print("bad run_async response", obj, file=sys.stderr)
  raise SystemExit(1)
print(obj["job_id"])
PY
)"

approval_id=""
deadline=$(( $(date +%s) + 20 ))
while [[ -z "${approval_id}" ]]; do
  now=$(date +%s)
  if [[ "${now}" -gt "${deadline}" ]]; then
    echo "timed out waiting for approval request" >&2
    exit 1
  fi
  approvals="$(curl -fsS --noproxy "*" --max-time 10 \
    "${DAEMON_URL}/api/v1/approvals?status=pending&trace_id=$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote('${TRACE_ID}'))
PY
)&tool_name=calculator")"
  approval_id="$(python3 - <<PY
import json
obj = json.loads(r'''${approvals}''')
items = obj.get("approvals") or []
print(items[0]["approval_id"] if items else "")
PY
)"
  [[ -z "${approval_id}" ]] && sleep 0.1
done

post_decision() {
  local member_id="$1"
  local member_role="$2"
  local decision="$3"
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/json" \
    -d "$(python3 - <<PY
import json
print(json.dumps({
  "member_id": "${member_id}",
  "member_role": "${member_role}",
  "decision": "${decision}"
}))
PY
)" \
    "${DAEMON_URL}/api/v1/approvals/${approval_id}/decisions" >/dev/null
}

post_decision "alice" "planner" "approve"
post_decision "bob" "planner" "approve"

detail_after_same_roles="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/approvals/${approval_id}")"
python3 - <<PY
import json, sys
obj = json.loads(r'''${detail_after_same_roles}''')
approval = obj.get("approval") or {}
if approval.get("status") != "pending":
  print("expected pending after same-role approvals", approval, file=sys.stderr)
  raise SystemExit(1)
PY

post_decision "carol" "reviewer" "approve"

deadline=$(( $(date +%s) + 60 ))
cursor=0
while true; do
  now=$(date +%s)
  if [[ "${now}" -gt "${deadline}" ]]; then
    echo "timed out waiting for job ${job_id}" >&2
    exit 1
  fi
  st="$(curl -fsS --noproxy "*" --max-time 10 \
    "${DAEMON_URL}/api/v1/job?job_id=$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote('${job_id}'))
PY
)&include_events=1&cursor=${cursor}&max_events=256")"
  set +e
  python3 - <<PY
import json, sys
obj = json.loads(r'''${st}''')
if not obj.get("ok"):
  print("job query failed", obj, file=sys.stderr)
  raise SystemExit(1)
status = obj.get("status")
if status in ("done","error"):
  res = obj.get("result") or {}
  if not res.get("ok"):
    print("run failed", res, file=sys.stderr)
    raise SystemExit(1)
  txt = (res.get("assistant_text") or "").strip()
  if txt != "OK":
    print("unexpected assistant_text", txt, file=sys.stderr)
    raise SystemExit(1)
  raise SystemExit(0)
raise SystemExit(2)
PY
  rc=$?
  set -e
  if [[ $rc -eq 0 ]]; then
    break
  elif [[ $rc -eq 1 ]]; then
    exit 1
  fi
  cursor="$(python3 - <<PY
import json
obj = json.loads(r'''${st}''')
print(int(obj.get("events_cursor_next") or 0))
PY
)"
  sleep 0.2
done

# Best-effort cleanup.
curl -fsS --noproxy "*" -X DELETE "${DAEMON_URL}/api/v1/job?job_id=$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote('${job_id}'))
PY
)" --max-time 5 >/dev/null || true

echo "${NAME} OK"
