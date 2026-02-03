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

SESSION_ID="agentd_stream_provider_retry_$(date +%s)_$RANDOM"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

# Start an OpenAI-compatible streaming stub that returns 429 once, then streams success.
python3 -u - <<PY > "${LOG_DIR}/agentd_stream_provider_retry_smoke.stub.stdout.log" 2> "${LOG_DIR}/agentd_stream_provider_retry_smoke.stub.stderr.log" &
import json
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

STATE = {"calls": 0}

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

    want_stream = bool(req.get("stream"))
    STATE["calls"] += 1

    if want_stream and STATE["calls"] == 1:
      body = {"error": {"message": "rate limited (simulated)"}}
      data = json.dumps(body).encode("utf-8")
      self.send_response(429)
      self.send_header("Content-Type", "application/json; charset=utf-8")
      self.send_header("Retry-After", "1")
      self.send_header("Content-Length", str(len(data)))
      self.end_headers()
      self.wfile.write(data)
      return

    if not want_stream:
      body = {
        "id": "cmpl_stub_nonstream",
        "object": "chat.completion",
        "created": 0,
        "model": "stub",
        "choices": [
          {"index": 0, "message": {"role": "assistant", "content": "Hello world END"}, "finish_reason": "stop"}
        ],
      }
      data = json.dumps(body).encode("utf-8")
      self.send_response(200)
      self.send_header("Content-Type", "application/json; charset=utf-8")
      self.send_header("Content-Length", str(len(data)))
      self.end_headers()
      self.wfile.write(data)
      return

    self.send_response(200)
    self.send_header("Content-Type", "text/event-stream")
    self.send_header("Cache-Control", "no-cache")
    self.send_header("Connection", "close")
    self.end_headers()

    chunks = [
      {"choices": [{"delta": {"content": "Hello "}}]},
      {"choices": [{"delta": {"content": "world END"}}]},
    ]
    for c in chunks:
      line = "data: " + json.dumps(c) + "\\n\\n"
      self.wfile.write(line.encode("utf-8"))
      self.wfile.flush()
      time.sleep(0.05)
    self.wfile.write(b"data: [DONE]\\n\\n")
    self.wfile.flush()

HTTPServer(("127.0.0.1", ${PORT_STUB}), H).serve_forever()
PY
STUB_PID=$!

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_stream_provider_retry_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

job_json="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "Write exactly: Hello world END",
  "session_id": "${SESSION_ID}",
  "tools": "none",
  "stream_assistant": True,
  "base_url": "${STUB_BASE}",
  "api_key": "dummy",
  "model": "stub",
  "trace": False,
  "verbose": False,
  "max_retries": 2,
  "retry_base_ms": 1,
  "retry_max_ms": 5,
  "retry_jitter": 0.0,
  "respect_retry_after": False
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

job_id_q="$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${job_id}")"
stream="$(curl -fsS --noproxy "*" --max-time 10 "${DAEMON_URL}/api/v1/job/stream?job_id=${job_id_q}&cursor=0" | head -n 8000)"

python3 - <<PY
import sys
s = r'''${stream}'''
has_done = ("event: job_done" in s)
has_retry = ('"type":"retry"' in s or '"type": "retry"' in s) and ('"scope":"provider"' in s or '"scope": "provider"' in s)
has_delta = ('"type":"assistant_delta"' in s or '"type": "assistant_delta"' in s)
if not has_done:
  print("missing job_done in SSE stream", file=sys.stderr)
  raise SystemExit(1)
if not has_retry:
  print("expected a provider retry event in SSE stream", file=sys.stderr)
  raise SystemExit(1)
if not has_delta:
  print("expected at least one assistant_delta event", file=sys.stderr)
  raise SystemExit(1)
PY

final="$(curl -fsS --noproxy "*" --max-time 10 "${DAEMON_URL}/api/v1/job?job_id=${job_id_q}")"

python3 - <<PY
import json, sys
obj=json.loads(r'''${final}''')
if not obj.get("ok") or obj.get("status") != "done":
  print("job did not complete", obj, file=sys.stderr)
  raise SystemExit(1)
res=obj.get("result") or {}
if not isinstance(res, dict) or not res.get("ok"):
  print("run result not ok", obj, file=sys.stderr)
  raise SystemExit(1)
txt=(res.get("assistant_text") or "").strip()
if txt != "Hello world END":
  print(f"unexpected assistant_text: {txt!r}", file=sys.stderr)
  raise SystemExit(1)
PY

# Cleanup.
sid_q="$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${SESSION_ID}")"
curl -fsS --noproxy "*" -X DELETE "${DAEMON_URL}/api/v1/session?session_id=${sid_q}" --max-time 10 >/dev/null || true
curl -fsS --noproxy "*" -X DELETE "${DAEMON_URL}/api/v1/job?job_id=${job_id_q}" --max-time 5 >/dev/null || true

