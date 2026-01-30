#!/usr/bin/env bash
set -euo pipefail

AGENTD_BIN="${1:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "missing agentd binary path arg" >&2
  exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
LOG_DIR="${PROJECT_ROOT}/build"
mkdir -p "${LOG_DIR}"

PORT_DAEMON="$(( (RANDOM % 20000) + 20000 ))"
PORT_STUB="$(( (RANDOM % 20000) + 20000 ))"
HOST="127.0.0.1"
DAEMON_URL="http://${HOST}:${PORT_DAEMON}"
STUB_BASE="http://${HOST}:${PORT_STUB}/v1"

AUTH_TOKEN="t"

cleanup() {
  if [[ -n "${AGENTD_PID:-}" ]]; then
    kill -TERM "${AGENTD_PID}" >/dev/null 2>&1 || true
    wait "${AGENTD_PID}" >/dev/null 2>&1 || true
  fi
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

python3 -u - <<PY > "${LOG_DIR}/agentd_sse_auth_smoke.stub.stdout.log" 2> "${LOG_DIR}/agentd_sse_auth_smoke.stub.stderr.log" &
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
    length = int(self.headers.get("content-length") or "0")
    _ = self.rfile.read(length) if length > 0 else b""
    body = {
      "id": "cmpl_test",
      "object": "chat.completion",
      "created": 0,
      "model": "stub",
      "choices": [
        {"index": 0, "message": {"role": "assistant", "content": "ok"}, "finish_reason": "stop"}
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

"${AGENTD_BIN}" \
  --host "${HOST}" \
  --port "${PORT_DAEMON}" \
  --auth-token "${AUTH_TOKEN}" \
  --base-url "${STUB_BASE}" \
  --api-key "dummy" \
  --model "stub" \
  --tools none \
  > "${LOG_DIR}/agentd_sse_auth_smoke.stdout.log" 2> "${LOG_DIR}/agentd_sse_auth_smoke.stderr.log" &
AGENTD_PID=$!

# Wait for health endpoint.
for _ in $(seq 1 60); do
  if curl -fsS --noproxy "*" --max-time 2 "${DAEMON_URL}/api/v1/health" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done
if ! curl -fsS --noproxy "*" --max-time 2 "${DAEMON_URL}/api/v1/health" >/dev/null 2>&1; then
  echo "agentd did not become healthy: ${DAEMON_URL}" >&2
  exit 1
fi

# Unauthorized stream should be rejected with 401.
code="$(curl -sS --noproxy "*" --max-time 3 -o /dev/null -w '%{http_code}' "${DAEMON_URL}/api/v1/job/stream?job_id=missing&cursor=0")"
if [[ "${code}" != "401" ]]; then
  echo "expected 401 for unauthenticated SSE, got ${code}" >&2
  exit 1
fi

# Start an async job (uses local stub provider, so no network).
job_json="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${AUTH_TOKEN}" \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "hello",
  "no_session": True,
  "tools": "none",
  "base_url": "${STUB_BASE}",
  "api_key": "dummy",
  "model": "stub"
}))
PY
)" \
  "${DAEMON_URL}/api/v1/run_async")"

job_id="$(python3 - <<PY
import json
obj = json.loads(r'''${job_json}''')
print(obj.get("job_id",""))
PY
)"
if [[ -z "${job_id}" ]]; then
  echo "missing job_id from run_async response: ${job_json}" >&2
  exit 1
fi

# Authenticated SSE stream should return job_done.
out="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Authorization: Bearer ${AUTH_TOKEN}" \
  "${DAEMON_URL}/api/v1/job/stream?job_id=$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote('${job_id}'))
PY
)&cursor=0")"

python3 - <<PY
import sys
s = r'''${out}'''
if "event: job_done" not in s:
  print("missing job_done event; got:", s, file=sys.stderr)
  raise SystemExit(1)
PY
