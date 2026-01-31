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

SESSION_ID="agentd_client_profile_injection_smoke_$(date +%s)_$RANDOM"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

# Stub provider that validates agentd injects host system prompt + webui client profile even when the session is non-empty.
# Sequence:
# 1) tools=none run => creates a non-empty session without host system prompts.
# 2) tools=host run => should include default host system prompt + CLIENT_PROFILE=webui in the provider request messages.
python3 -u - <<PY > "${LOG_DIR}/agentd_client_profile_injection_smoke.stub.stdout.log" 2> "${LOG_DIR}/agentd_client_profile_injection_smoke.stub.stderr.log" &
import json
from http.server import BaseHTTPRequestHandler, HTTPServer

REQ_COUNT = 0

def find_substr(messages, role, needle):
  for m in messages:
    if not isinstance(m, dict):
      continue
    if m.get("role") != role:
      continue
    c = m.get("content")
    if isinstance(c, str) and needle in c:
      return True
  return False

def any_content_substr(messages, needle):
  for m in messages:
    if not isinstance(m, dict):
      continue
    c = m.get("content")
    if isinstance(c, str) and needle in c:
      return True
  return False

class H(BaseHTTPRequestHandler):
  def log_message(self, fmt, *args):
    return

  def do_POST(self):
    global REQ_COUNT
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

    REQ_COUNT += 1
    messages = req.get("messages") if isinstance(req.get("messages"), list) else []

    # First request: accept and reply trivially (tools=none).
    if REQ_COUNT == 1:
      body = {
        "id": "cmpl_stub_1",
        "object": "chat.completion",
        "created": 0,
        "model": "stub",
        "choices": [
          {"index": 0, "message": {"role": "assistant", "content": "seed ok"}, "finish_reason": "stop"}
        ],
      }
      data = json.dumps(body).encode("utf-8")
      self.send_response(200)
      self.send_header("Content-Type", "application/json; charset=utf-8")
      self.send_header("Content-Length", str(len(data)))
      self.end_headers()
      self.wfile.write(data)
      return

    # Second request: validate host+profile system prompts are present in the actual provider request.
    try:
      if not any_content_substr(messages, "seed: create a non-empty session without tools"):
        raise AssertionError("expected prior seed prompt in messages (non-empty session)")
      if not find_substr(messages, "system", "You are a host-side coding agent"):
        raise AssertionError("missing default host system prompt in messages")
      if not find_substr(messages, "system", "CLIENT_PROFILE=webui"):
        raise AssertionError("missing CLIENT_PROFILE=webui in messages")
    except Exception as e:
      err = {"error": str(e), "req_count": REQ_COUNT, "roles": [m.get("role") for m in messages if isinstance(m, dict)]}
      data = json.dumps(err).encode("utf-8")
      self.send_response(500)
      self.send_header("Content-Type", "application/json; charset=utf-8")
      self.send_header("Content-Length", str(len(data)))
      self.end_headers()
      self.wfile.write(data)
      return

    body = {
      "id": "cmpl_stub_2",
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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_client_profile_injection_smoke" \
  --tools host \
  --yolo \
  --host-scope "${LOG_DIR}"

agentd_smoke_wait_health "${DAEMON_URL}"

export SESSION_ID
export STUB_BASE

# 1) Create a non-empty session with tools=none (so no host system prompts are inserted at creation time).
resp1="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json, os
print(json.dumps({
  "prompt": "seed: create a non-empty session without tools",
  "session_id": os.environ["SESSION_ID"],
  "tools": "none",
  "base_url": os.environ["STUB_BASE"],
  "api_key": "dummy",
  "model": "stub",
  "client": {"kind":"webui","id":"webui-smoke","instance_id":"tab-smoke"},
}))
PY
)" \
  "${DAEMON_URL}/api/v1/run")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp1}''')
if not obj.get("ok"):
  print("seed run failed:", obj, file=sys.stderr)
  raise SystemExit(1)
PY

# 2) Run again with tools=host. The provider request must include the host system prompt + webui profile.
resp2="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json, os
print(json.dumps({
  # Keep this prompt intentionally non-effectful: this test is about system prompt injection,
  # not about tool-call behavior or client-side media/scene side effects.
  "prompt": "hello (host tools run; verify injected system prompts)",
  "session_id": os.environ["SESSION_ID"],
  "tools": "host",
  "yolo": True,
  "tools_root": "@host",
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
obj = json.loads(r'''${resp2}''')
if not obj.get("ok"):
  print("host run failed:", obj, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_client_profile_injection_smoke OK"
