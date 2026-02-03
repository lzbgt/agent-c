#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
PLUGIN_PATH="${2:-}"
if [[ -z "${AGENTD_BIN}" || -z "${PLUGIN_PATH}" ]]; then
  echo "usage: agentd_tool_plugin_smoke.sh <agentd_bin> <plugin_path>" >&2
  exit 2
fi
if [[ ! -f "${PLUGIN_PATH}" ]]; then
  echo "plugin not found: ${PLUGIN_PATH}" >&2
  exit 2
fi

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

PORT_DAEMON="$(agentd_smoke_pick_port)"
PORT_STUB="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"
STUB_BASE="http://${HOST}:${PORT_STUB}/v1"
NAME="agentd_tool_plugin_smoke"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

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
                    "name": "ext_echo",
                    "arguments": json.dumps({"text": "hello from stub"}),
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
  --tools host \
  --no-yolo \
  --tool-plugin "${PLUGIN_PATH}"

agentd_smoke_wait_health "${DAEMON_URL}"

tools_json="$(curl -fsS --noproxy "*" --max-time 10 "${DAEMON_URL}/api/v1/tools?tools=host&yolo=0&host_policy=readonly")"
python3 - <<PY
import json, sys
obj = json.loads(r'''${tools_json}''')
if not obj.get("ok"):
  print("tools endpoint failed", obj, file=sys.stderr)
  raise SystemExit(1)
defs = obj.get("defs") or []
names = [t.get("name") for t in defs if isinstance(t, dict)]
if "ext_echo" not in names:
  print("ext_echo not listed; names=", names, file=sys.stderr)
  raise SystemExit(1)
PY

resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "Call ext_echo then say OK",
  "no_session": True,
  "tools": "host",
  "yolo": False,
  "host_policy": "readonly",
  "base_url": "${STUB_BASE}",
  "api_key": "dummy",
  "model": "stub",
  "max_steps": 4,
  "verbose": True
}))
PY
)" \
  "${DAEMON_URL}/api/v1/run")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp}''')
if not obj.get("ok"):
  print("run failed:", obj, file=sys.stderr)
  raise SystemExit(1)
events = obj.get("events") or []
tool_results = [e for e in events if isinstance(e, dict) and e.get("type") == "tool_result" and (e.get("data") or {}).get("tool_name") == "ext_echo"]
if not tool_results:
  types = [e.get("type") for e in events if isinstance(e, dict)]
  print("missing ext_echo tool_result; event types=", types, file=sys.stderr)
  raise SystemExit(1)
content = (tool_results[-1].get("data") or {}).get("content") or ""
if "hello from stub" not in content:
  print("unexpected tool_result content:", content, file=sys.stderr)
  raise SystemExit(1)
txt = (obj.get("assistant_text") or "").strip()
if txt != "OK":
  print("unexpected assistant_text:", txt, file=sys.stderr)
  raise SystemExit(1)
PY
