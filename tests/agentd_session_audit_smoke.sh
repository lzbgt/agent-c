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

SESSION_ID="agentd_audit_$(date +%s)_$RANDOM"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

# Start a tiny OpenAI-compatible stub server for tools=none runs.
python3 -u - <<PY > "${LOG_DIR}/agentd_session_audit_smoke.stub.stdout.log" 2> "${LOG_DIR}/agentd_session_audit_smoke.stub.stderr.log" &
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
      "id": "cmpl_stub_audit",
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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_session_audit_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "hello audit",
  "session_id": "${SESSION_ID}",
  "tools": "none",
  "base_url": "${STUB_BASE}",
  "api_key": "dummy",
  "model": "stub",
  "max_chars": 20000,
  "keep_last": 16,
  "verbose": True,
  "trace": False
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
txt = (obj.get("assistant_text") or "").strip()
if txt != "OK":
  print("unexpected assistant_text:", txt, file=sys.stderr)
  raise SystemExit(1)
PY

sid_q="$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote("${SESSION_ID}"))
PY
)"

audit="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/session/audit?session_id=${sid_q}&max_bytes=1048576")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${audit}''')
if not obj.get("ok"):
  print("audit endpoint failed:", obj, file=sys.stderr)
  raise SystemExit(1)
entries = obj.get("entries")
if not isinstance(entries, list) or not entries:
  print("expected non-empty entries array", obj, file=sys.stderr)
  raise SystemExit(1)
ok = False
for e in entries:
  if not isinstance(e, dict):
    continue
  if e.get("session_id") != "${SESSION_ID}":
    continue
  if e.get("prompt") != "hello audit":
    continue
  if e.get("assistant_text") != "OK":
    continue
  if e.get("tools") != "none":
    continue
  if e.get("ok") is not True:
    continue
  ok = True
  break
if not ok:
  print("did not find expected audit entry", entries[-3:], file=sys.stderr)
  raise SystemExit(1)
PY

# Cleanup session artifacts.
curl -fsS --noproxy "*" -X DELETE "${DAEMON_URL}/api/v1/session?session_id=${sid_q}" --max-time 10 >/dev/null || true

