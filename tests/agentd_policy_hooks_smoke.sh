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

HOST="127.0.0.1"
PORT_STUB="$(agentd_smoke_pick_port)"
STUB_BASE="http://${HOST}:${PORT_STUB}/v1"
PROJECT_ROOT="$(agentd_smoke_project_root)"
README_PATH="${PROJECT_ROOT}/README.md"
export README_PATH

POLICY_TOOL="fs_read"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

# OpenAI-compatible stub:
# - first response: tool call to fs_read README.md
# - second response: assistant "OK" after tool result
python3 -u - <<PY > "${LOG_DIR}/agentd_policy_hooks_smoke.stub.stdout.log" 2> "${LOG_DIR}/agentd_policy_hooks_smoke.stub.stderr.log" &
import json
import os
from http.server import BaseHTTPRequestHandler, HTTPServer

README_PATH = os.environ.get("README_PATH", "README.md")

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
                    "name": "fs_read",
                    "arguments": json.dumps({"path": README_PATH, "max_lines": 10, "max_chars": 20000}),
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

run_case() {
  local mode="$1"
  local expect_ok="$2"
  local expect_enforced="$3"
  local label="$4"

  local port
  port="$(agentd_smoke_pick_port)"

  agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${port}" "agentd_policy_hooks_${label}" \
    --tools host \
    --no-yolo \
    --policy-mode "${mode}" \
    --policy-tool-deny "${POLICY_TOOL}"

  agentd_smoke_wait_health "${DAEMON_URL}"

  resp="$(curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/json" \
    -d "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "Read README.md then say OK",
  "no_session": True,
  "tools": "host",
  "yolo": False,
  "base_url": "${STUB_BASE}",
  "api_key": "dummy",
  "model": "stub",
  "max_steps": 3,
  "require_tool_call": True,
  "verbose": True
}))
PY
)" \
    "${DAEMON_URL}/api/v1/run")"

  python3 - <<PY
import json, sys
obj = json.loads(r'''${resp}''')
expect_ok = bool(${expect_ok})
expect_enforced = bool(${expect_enforced})
policy_tool = "${POLICY_TOOL}"
mode = "${mode}"

if expect_ok and not obj.get("ok"):
  print("expected ok=true, got false:", obj, file=sys.stderr)
  raise SystemExit(1)
if not expect_ok and obj.get("ok"):
  print("expected ok=false, got true:", obj, file=sys.stderr)
  raise SystemExit(1)
if not expect_ok:
  err = obj.get("error") or obj.get("err") or ""
  if "policy denied tool" not in err:
    print("expected policy error, got:", err, file=sys.stderr)
    raise SystemExit(1)
else:
  txt = (obj.get("assistant_text") or "").strip()
  if txt != "OK":
    print("unexpected assistant_text:", txt, file=sys.stderr)
    raise SystemExit(1)

events = obj.get("events") or []
policy_events = [e for e in events if isinstance(e, dict) and e.get("type") == "policy_decision"]
if not policy_events:
  print("missing policy_decision events", file=sys.stderr)
  raise SystemExit(1)
denies = []
for e in policy_events:
  data = e.get("data") or {}
  if data.get("action") == "deny" and data.get("tool_name") == policy_tool:
    denies.append(data)
if not denies:
  print("missing policy deny event for tool", policy_tool, file=sys.stderr)
  raise SystemExit(1)
if any(d.get("mode") != mode for d in denies):
  print("policy mode mismatch in deny events", denies, file=sys.stderr)
  raise SystemExit(1)
if any(d.get("enforced") != expect_enforced for d in denies):
  print("policy enforced mismatch in deny events", denies, file=sys.stderr)
  raise SystemExit(1)
PY

  agentd_smoke_stop
}

run_case "enforce" 0 1 "enforce"
run_case "audit" 1 0 "audit"

echo "agentd_policy_hooks_smoke OK"
