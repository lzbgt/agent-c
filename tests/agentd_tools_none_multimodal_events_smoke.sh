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
NAME="agentd_tools_none_multimodal_events_smoke"

MM_JSON='{"images":[{"mime":"image/png","b64":"AAA","name":"plot.png"}]}'
USER_TEXT="Please describe the plot."
ASSIST_TEXT="Here is the plot."

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

MM_JSON="${MM_JSON}" ASSIST_TEXT="${ASSIST_TEXT}" python3 -u - <<PY > "${LOG_DIR}/${NAME}.stub.stdout.log" 2> "${LOG_DIR}/${NAME}.stub.stderr.log" &
import json
import os
from http.server import BaseHTTPRequestHandler, HTTPServer

MM_JSON = os.environ["MM_JSON"]
ASSIST_TEXT = os.environ["ASSIST_TEXT"]
CONTENT = "__AGENT_MM_V1__" + MM_JSON + "\n" + ASSIST_TEXT

class H(BaseHTTPRequestHandler):
  def log_message(self, fmt, *args):
    return

  def do_POST(self):
    if self.path != "/v1/chat/completions":
      self.send_response(404)
      self.end_headers()
      return
    body = {
      "id": "cmpl_stub_mm",
      "object": "chat.completion",
      "created": 0,
      "model": "stub",
      "choices": [
        {"index": 0, "message": {"role": "assistant", "content": CONTENT}, "finish_reason": "stop"}
      ],
    }
    data = json.dumps(body).encode("utf-8")
    self.send_response(200)
    self.send_header("Content-Type", "application/json; charset=utf-8")
    self.send_header("Content-Length", str(len(data)))
    self.end_headers()
    self.wfile.write(data)

HTTPServer(("127.0.0.1", int(${PORT_STUB})), H).serve_forever()
PY
STUB_PID=$!

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(MM_JSON="${MM_JSON}" USER_TEXT="${USER_TEXT}" python3 - <<PY
import json
import os
mm_json = os.environ["MM_JSON"]
user_text = os.environ["USER_TEXT"]
prompt = "__AGENT_MM_V1__" + mm_json + "\\n" + user_text
print(json.dumps({
  "prompt": prompt,
  "session_id": "${NAME}",
  "tools": "none",
  "base_url": "${STUB_BASE}",
  "api_key": "dummy",
  "model": "stub",
  "stream_assistant": False,
  "max_chars": 20000,
  "keep_last": 16,
  "verbose": False,
  "trace": False
}))
PY
)" \
  "${DAEMON_URL}/api/v1/run")"

MM_JSON="${MM_JSON}" USER_TEXT="${USER_TEXT}" ASSIST_TEXT="${ASSIST_TEXT}" python3 - <<PY
import json
import os
import sys

mm_json = os.environ["MM_JSON"]
user_text = os.environ["USER_TEXT"]
assist_text = os.environ["ASSIST_TEXT"]

obj = json.loads(r'''${resp}''')
if not obj.get("ok"):
  print("run failed:", obj, file=sys.stderr)
  raise SystemExit(1)
if (obj.get("assistant_text") or "").strip() != assist_text:
  print("unexpected assistant_text:", obj.get("assistant_text"), file=sys.stderr)
  raise SystemExit(1)

events = obj.get("events") or []
user_ev = None
assistant_ev = None
for ev in events:
  if not isinstance(ev, dict):
    continue
  if ev.get("type") == "user_message":
    user_ev = ev
  if ev.get("type") == "assistant_message":
    assistant_ev = ev
if user_ev is None:
  print("missing user_message event", events, file=sys.stderr)
  raise SystemExit(1)
if assistant_ev is None:
  print("missing assistant_message event", events, file=sys.stderr)
  raise SystemExit(1)

ud = user_ev.get("data") or {}
if ud.get("user_content") != user_text:
  print("unexpected user_content", ud, file=sys.stderr)
  raise SystemExit(1)
um = json.loads(ud.get("user_mm_json") or "{}")
if (um.get("images") or [{}])[0].get("name") != "plot.png":
  print("unexpected user_mm_json", um, file=sys.stderr)
  raise SystemExit(1)
if ud.get("user_mm_truncated") not in (0, 0.0):
  print("unexpected user_mm_truncated", ud, file=sys.stderr)
  raise SystemExit(1)
if ud.get("user_mm_bytes") != len(ud.get("user_mm_json") or ""):
  print("unexpected user_mm_bytes", ud, file=sys.stderr)
  raise SystemExit(1)

ad = assistant_ev.get("data") or {}
if ad.get("assistant_content") != assist_text:
  print("unexpected assistant_content", ad, file=sys.stderr)
  raise SystemExit(1)
if ad.get("has_tool_calls") not in (0, 0.0):
  print("unexpected has_tool_calls", ad, file=sys.stderr)
  raise SystemExit(1)
am = json.loads(ad.get("assistant_mm_json") or "{}")
if (am.get("images") or [{}])[0].get("name") != "plot.png":
  print("unexpected assistant_mm_json", am, file=sys.stderr)
  raise SystemExit(1)
if ad.get("assistant_mm_truncated") not in (0, 0.0):
  print("unexpected assistant_mm_truncated", ad, file=sys.stderr)
  raise SystemExit(1)
if ad.get("assistant_mm_bytes") != len(ad.get("assistant_mm_json") or ""):
  print("unexpected assistant_mm_bytes", ad, file=sys.stderr)
  raise SystemExit(1)
PY

echo "${NAME} OK"
