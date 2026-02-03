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

NAME="agentd_job_restart_durability_smoke"
DB_PATH="${LOG_DIR}/${NAME}_${PORT_DAEMON}.sqlite"
STATE_DIR="${LOG_DIR}/${NAME}_${PORT_DAEMON}.state"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

# Start an OpenAI-compatible stub that deliberately responds slowly so the daemon can be restarted mid-job.
python3 -u - <<PY > "${LOG_DIR}/${NAME}.stub.stdout.log" 2> "${LOG_DIR}/${NAME}.stub.stderr.log" &
import json
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

class H(BaseHTTPRequestHandler):
  def log_message(self, fmt, *args):
    return

  def do_POST(self):
    if self.path != "/v1/chat/completions":
      self.send_response(404)
      self.end_headers()
      return
    # Delay long enough that the daemon is killed mid-request.
    time.sleep(8.0)
    body = {
      "id": "cmpl_stub_nonstream",
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

# Start agentd (first process lifetime).
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}" \
  --db-path "${DB_PATH}" \
  --state-dir "${STATE_DIR}" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

job_json="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "Say OK",
  "no_session": True,
  "tools": "none",
  "base_url": "${STUB_BASE}",
  "api_key": "dummy",
  "model": "stub",
  "timeout_ms": 20000,
  "max_retries": 0,
  "trace": False,
  "verbose": False
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

# Give the background thread a moment to start, then restart the daemon mid-job.
sleep 0.5
agentd_smoke_stop

# Restart agentd (second process lifetime) against the same DB.
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}_restart" \
  --db-path "${DB_PATH}" \
  --state-dir "${STATE_DIR}" \
  --tools none
agentd_smoke_wait_health "${DAEMON_URL}"

job_id_q="$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${job_id}")"
job="$(curl -fsS --noproxy "*" --max-time 10 "${DAEMON_URL}/api/v1/job?job_id=${job_id_q}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${job}''')
if not obj.get("ok"):
  print("expected ok job lookup after restart", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("status") != "interrupted":
  print("expected status=interrupted after restart", obj, file=sys.stderr)
  raise SystemExit(1)
res = obj.get("result") or {}
if not isinstance(res, dict):
  print("expected result object", obj, file=sys.stderr)
  raise SystemExit(1)
if res.get("ok") is not False:
  print("expected result.ok=false", obj, file=sys.stderr)
  raise SystemExit(1)
PY

