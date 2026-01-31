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

SESSION_ID="agentd_missing_tool_calls_smoke_$(date +%s)_$RANDOM"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

# Stub provider that always returns an assistant message with no tool calls.
python3 -u - <<PY > "${LOG_DIR}/agentd_missing_tool_calls_smoke.stub.stdout.log" 2> "${LOG_DIR}/agentd_missing_tool_calls_smoke.stub.stderr.log" &
import json
from http.server import BaseHTTPRequestHandler, HTTPServer

class H(BaseHTTPRequestHandler):
  def log_message(self, fmt, *args):
    return

  def do_POST(self):
    if self.path != "/v1/chat/completions":
      self.send_response(404)
      self.end_headers()
      return

    body = {
      "id": "cmpl_stub",
      "object": "chat.completion",
      "created": 0,
      "model": "stub",
      "choices": [
        {"index": 0, "message": {"role": "assistant", "content": "OK"}, "finish_reason": "stop"}
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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_missing_tool_calls_smoke" \
  --tools host \
  --yolo \
  --host-scope "${LOG_DIR}"

agentd_smoke_wait_health "${DAEMON_URL}"

export SESSION_ID
export STUB_BASE

resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json, os
print(json.dumps({
  "prompt": "say hello in audio file play for me. use the webui presentation",
  "session_id": os.environ["SESSION_ID"],
  "tools": "host",
  "yolo": True,
  "tools_root": "@host",
  "require_tool_call": True,
  "base_url": os.environ["STUB_BASE"],
  "api_key": "dummy",
  "model": "stub",
  "max_steps": 2,
  "client": {"kind":"webui","id":"webui-smoke","instance_id":"tab-smoke"},
}))
PY
)" \
  "${DAEMON_URL}/api/v1/run")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp}''')

if obj.get("ok") is True:
  print("expected ok=false (require_tool_call), got ok=true:", obj, file=sys.stderr)
  raise SystemExit(1)

events = obj.get("events") if isinstance(obj.get("events"), list) else []
reasons = []
done_reason = None
for ev in events:
  if not isinstance(ev, dict):
    continue
  t = ev.get("type")
  d = ev.get("data") if isinstance(ev.get("data"), dict) else {}
  if t == "error":
    r = d.get("reason")
    if isinstance(r, str):
      reasons.append(r)
  if t == "done":
    r = d.get("reason")
    if isinstance(r, str):
      done_reason = r

if "require_tool_call_unsatisfied" not in reasons:
  print("expected error.reason=require_tool_call_unsatisfied in events; got:", reasons, file=sys.stderr)
  raise SystemExit(1)

if done_reason is not None and done_reason not in ("require_tool_call_unsatisfied", "no tool calls", "no tool call occurred"):
  print("expected done.reason to reflect require_tool_call failure when present; got:", done_reason, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_missing_tool_calls_smoke OK"
