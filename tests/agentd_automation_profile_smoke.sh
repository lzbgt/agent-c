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

python3 -u - <<PY > "${LOG_DIR}/agentd_automation_profile_smoke.stub.stdout.log" 2> "${LOG_DIR}/agentd_automation_profile_smoke.stub.stderr.log" &
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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_automation_profile_smoke" \
  --tools host \
  --yolo

agentd_smoke_wait_health "${DAEMON_URL}"

caps="$(curl -fsS --noproxy "*" --max-time 5 "${DAEMON_URL}/api/v1/caps")"
python3 - <<PY
import json, sys
obj = json.loads(r'''${caps}''')
if not obj.get("ok"):
  print(obj, file=sys.stderr)
  raise SystemExit(1)
features = obj.get("features") or {}
auto = features.get("automation") or {}
if auto.get("default_profile") not in ("full", "guided", "strict", "custom"):
  print("unexpected automation default_profile:", auto.get("default_profile"), file=sys.stderr)
  raise SystemExit(1)
profiles = auto.get("profiles") or []
need = {"full", "guided", "strict", "custom"}
if not need.issubset(set(profiles)):
  print("missing automation profiles:", sorted(need - set(profiles)), file=sys.stderr)
  raise SystemExit(1)
if auto.get("per_run_override") not in (True, 1):
  print("unexpected per_run_override:", auto.get("per_run_override"), file=sys.stderr)
  raise SystemExit(1)
PY

resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "hello",
  "tools": "host",
  "automation_profile": "guided",
  "yolo": True,
  "host_policy": "full",
  "base_url": "${STUB_BASE}",
  "api_key": "dummy",
  "model": "stub",
  "max_steps": 1
}))
PY
)" \
  "${DAEMON_URL}/api/v1/run")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp}''')
if not obj.get("ok"):
  print("run not ok:", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("effective_automation_profile") != "guided":
  print("unexpected effective_automation_profile:", obj.get("effective_automation_profile"), file=sys.stderr)
  raise SystemExit(1)
if obj.get("effective_yolo") not in (False, 0):
  print("unexpected effective_yolo:", obj.get("effective_yolo"), file=sys.stderr)
  raise SystemExit(1)
if obj.get("effective_host_policy") != "readonly":
  print("unexpected effective_host_policy:", obj.get("effective_host_policy"), file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_automation_profile_smoke OK"
