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
NAME="agentd_tool_server_esp32_bridge_smoke"
MANIFEST_PATH="${LOG_DIR}/${NAME}_${PORT_DAEMON}.manifest.json"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

cat > "${MANIFEST_PATH}" <<'JSON'
{
  "spec_version": "um-acds/0.1",
  "manifest_version": "0.0.1",
  "caps_sha256": "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "node": {"node_id": "esp32_lab_bridge_1"},
  "runtime": {"agent_core": {"version": "0.0.0"}},
  "hardware": {"presence": {"home.light.switch": "present"}},
  "tools": [
    {
      "name": "home.light.switch",
      "description": "Switch a lab light through an ESP32 bridge.",
      "kind": "actuator",
      "parameters_schema": {
        "type": "object",
        "additionalProperties": false,
        "properties": {
          "state": {"type": "string", "enum": ["on", "off"]},
          "reason": {"type": "string"}
        },
        "required": ["state"]
      },
      "timeout_ms": 1000,
      "idempotent": false,
      "side_effect_level": "low",
      "hazards": ["visible_light"]
    }
  ],
  "safety": {},
  "tags": ["room:lab"]
}
JSON

# Provider stub verifies the daemon sees the provider-safe tool name, then forces
# one bridge tool call. The bridge must preserve the dotted UM-ACDS name inside
# the returned TASK_ASSIGN.
python3 -u - <<PY > "${LOG_DIR}/${NAME}.stub.stdout.log" 2> "${LOG_DIR}/${NAME}.stub.stderr.log" &
import json
from http.server import BaseHTTPRequestHandler, HTTPServer

def has_tool_result(messages):
  for m in messages:
    if isinstance(m, dict) and m.get("role") == "tool":
      return True
  return False

def send_json(handler, status, body):
  data = json.dumps(body).encode("utf-8")
  handler.send_response(status)
  handler.send_header("Content-Type", "application/json; charset=utf-8")
  handler.send_header("Content-Length", str(len(data)))
  handler.end_headers()
  handler.wfile.write(data)

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
      send_json(self, 200, {
        "id": "cmpl_stub_2",
        "object": "chat.completion",
        "created": 0,
        "model": "stub",
        "choices": [
          {"index": 0, "message": {"role": "assistant", "content": "OK"}, "finish_reason": "stop"}
        ],
      })
      return

    tool_names = []
    for item in req.get("tools") or []:
      if isinstance(item, dict):
        fn = item.get("function") if isinstance(item.get("function"), dict) else {}
        if isinstance(fn.get("name"), str):
          tool_names.append(fn["name"])
    if "esp32_home_light_switch" not in tool_names or "home.light.switch" in tool_names:
      send_json(self, 400, {"error": "missing provider-safe ESP32 bridge tool", "tool_names": tool_names})
      return

    send_json(self, 200, {
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
                  "name": "esp32_home_light_switch",
                  "arguments": json.dumps({"state": "on", "reason": "agentd-smoke"}),
                },
              }
            ],
          },
          "finish_reason": "tool_calls",
        }
      ],
    })

HTTPServer(("127.0.0.1", ${PORT_STUB}), H).serve_forever()
PY
STUB_PID=$!

TOOL_SERVER_CMD="python3 -u ./tools/tool_server_esp32_bridge.py --manifest ${MANIFEST_PATH} --transport dry-run"

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}" \
  --tools basic \
  --tool-server-cmd "${TOOL_SERVER_CMD}" \
  --no-yolo

agentd_smoke_wait_health "${DAEMON_URL}"

tools_resp="$(curl -fsS --noproxy "*" --max-time 10 "${DAEMON_URL}/api/v1/tools")"
python3 - <<PY
import json, sys
obj = json.loads(r'''${tools_resp}''')
if not obj.get("ok"):
  print("tools list failed", obj, file=sys.stderr)
  raise SystemExit(1)
defs = obj.get("defs") or []
names = {d.get("name") for d in defs if isinstance(d, dict)}
if "esp32_home_light_switch" not in names:
  print("missing provider-safe ESP32 bridge tool", names, file=sys.stderr)
  raise SystemExit(1)
if "home.light.switch" in names:
  print("dotted UM-ACDS name leaked into provider tool names", names, file=sys.stderr)
  raise SystemExit(1)
PY

resp="$(curl -fsS --noproxy "*" --max-time 15 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "Call the ESP32 bridge light switch tool, then say OK",
  "no_session": True,
  "tools": "basic",
  "yolo": False,
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
hit = False
for e in events:
  if not isinstance(e, dict) or e.get("type") != "tool_result":
    continue
  d = e.get("data") if isinstance(e.get("data"), dict) else {}
  if d.get("tool_name") != "esp32_home_light_switch":
    continue
  content = d.get("content") if isinstance(d.get("content"), str) else ""
  compact = content.replace(" ", "")
  if '"type":"TASK_ASSIGN"' in compact and '"tool":"home.light.switch"' in compact and '"agentd_tool":"esp32_home_light_switch"' in compact:
    hit = True
    break
if not hit:
  print("missing ESP32 bridge TASK_ASSIGN content in tool_result events", events, file=sys.stderr)
  raise SystemExit(1)
txt = (obj.get("assistant_text") or "").strip()
if txt != "OK":
  print("unexpected assistant_text:", txt, file=sys.stderr)
  raise SystemExit(1)
PY

echo "${NAME} OK"
