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

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

# OpenAI-compatible stub: always returns assistant content "OK".
python3 -u - <<PY > "${LOG_DIR}/agentd_orchestrate_smoke.stub.stdout.log" 2> "${LOG_DIR}/agentd_orchestrate_smoke.stub.stderr.log" &
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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_orchestrate_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
tasks = [
  {"task_id":"t1","request":{"prompt":"One","no_session":True,"tools":"none","base_url":"${STUB_BASE}","api_key":"dummy","model":"stub","trace":False}},
  {"task_id":"t2","request":{"prompt":"Two","no_session":True,"tools":"none","base_url":"${STUB_BASE}","api_key":"dummy","model":"stub","trace":False}},
]
print(json.dumps({"tasks": tasks, "max_concurrency": 2}))
PY
)" \
  "${DAEMON_URL}/api/v1/orchestrate")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp}''')
if not obj.get("ok"):
  print("orchestrate failed", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("all_ok") is not True:
  print("expected all_ok=true", obj, file=sys.stderr)
  raise SystemExit(1)
res = obj.get("results") or []
if not isinstance(res, list) or len(res) != 2:
  print("expected 2 results", obj, file=sys.stderr)
  raise SystemExit(1)
for r in res:
  if not isinstance(r, dict) or not r.get("ok"):
    print("task not ok", r, file=sys.stderr)
    raise SystemExit(1)
  rr = r.get("result") or {}
  if not isinstance(rr, dict) or rr.get("ok") is not True:
    print("expected inner run result ok", r, file=sys.stderr)
    raise SystemExit(1)
  if (rr.get("assistant_text") or "").strip() != "OK":
    print("unexpected assistant_text", rr.get("assistant_text"), file=sys.stderr)
    raise SystemExit(1)
PY

