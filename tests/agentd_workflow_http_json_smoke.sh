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
NAME="agentd_workflow_http_json_smoke"

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

# Local stub HTTP server for http_json:
# - POST /echo returns {"ok":true,"echo":<request_json>}
python3 -u - <<PY > "${LOG_DIR}/${NAME}.stub.stdout.log" 2> "${LOG_DIR}/${NAME}.stub.stderr.log" &
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

class H(BaseHTTPRequestHandler):
  def log_message(self, fmt, *args):
    return

  def do_POST(self):
    if self.path != "/echo":
      self.send_response(404)
      self.end_headers()
      return
    length = int(self.headers.get("content-length") or "0")
    raw = self.rfile.read(length) if length > 0 else b"{}"
    try:
      req = json.loads(raw.decode("utf-8"))
    except Exception:
      req = {"_parse": "error"}

    body = {"ok": True, "echo": req}
    data = json.dumps(body).encode("utf-8")
    self.send_response(200)
    self.send_header("Content-Type", "application/json; charset=utf-8")
    self.send_header("Content-Length", str(len(data)))
    self.end_headers()
    self.wfile.write(data)

ThreadingHTTPServer(("127.0.0.1", ${PORT_STUB}), H).serve_forever()
PY
STUB_PID=$!

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}" \
  --db-path "${DB_PATH}" \
  --state-dir "${STATE_DIR}" \
  --tools none \
  --workflow-enable-http-tasks

agentd_smoke_wait_health "${DAEMON_URL}"

# Sanity: config endpoint reports the gate as enabled.
cfg_resp="$(curl -fsS --noproxy "*" --max-time 10 "${DAEMON_URL}/api/v1/config")"
python3 - <<PY
import json, sys
obj = json.loads(r'''${cfg_resp}''')
if not obj.get("ok"):
  print("config failed", obj, file=sys.stderr)
  raise SystemExit(1)
eng = obj.get("engines") or {}
if not eng.get("workflow_enable_http_tasks"):
  print("expected workflow_enable_http_tasks=true", eng, file=sys.stderr)
  raise SystemExit(1)
PY

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
tasks = [
  {
    "task_id": "H",
    "kind": "http_json",
    "http_json": {
      "url": "http://${HOST}:${PORT_STUB}/echo",
      "method": "POST",
      "timeout_ms": 5000,
      "max_bytes": 65536,
      "body": {"ping": "pong"}
    }
  }
]
print(json.dumps({"tasks": tasks}))
PY
)" \
  "${DAEMON_URL}/api/v1/workflow/submit")"

workflow_id="$(python3 - <<PY
import json
obj = json.loads(r'''${submit_resp}''')
print(obj.get("workflow_id",""))
PY
)"
if [[ -z "${workflow_id}" ]]; then
  echo "failed to get workflow_id: ${submit_resp}" >&2
  exit 1
fi

final=""
for _ in $(seq 1 200); do
  final="$(curl -fsS --noproxy "*" --max-time 5 \
    "${DAEMON_URL}/api/v1/workflow?workflow_id=${workflow_id}&include_tasks=1&include_results=1")"
  if python3 - <<PY >/dev/null 2>&1
import json, sys
obj = json.loads(r'''${final}''')
w = obj.get("workflow") or {}
st = w.get("status")
if st in ("done","error","cancelled"):
  raise SystemExit(0)
raise SystemExit(1)
PY
  then
    break
  fi
  sleep 0.05
done

python3 - <<PY
import json, sys
obj = json.loads(r'''${final}''')
if not obj.get("ok"):
  print("workflow get failed", obj, file=sys.stderr)
  raise SystemExit(1)
w = obj.get("workflow") or {}
if w.get("status") != "done":
  print("expected workflow status done", w, file=sys.stderr)
  raise SystemExit(1)
tasks = obj.get("tasks") or []
by_id = {t.get("task_id"): t for t in tasks if isinstance(t, dict)}
if by_id.get("H", {}).get("status") != "done":
  print("task not done", by_id.get("H"), file=sys.stderr)
  raise SystemExit(1)

res = obj.get("result") or {}
by = res.get("results_by_task") or {}
r = by.get("H") or {}
if not r.get("ok"):
  print("http_json result not ok", r, file=sys.stderr)
  raise SystemExit(1)
http = r.get("http") or {}
if http.get("status") != 200:
  print("expected status 200", http, file=sys.stderr)
  raise SystemExit(1)
sha = http.get("response_sha256") or ""
if not (isinstance(sha, str) and sha.startswith("sha256:") and len(sha) == 71):
  print("expected http.response_sha256 sha256:<64hex>", sha, file=sys.stderr)
  raise SystemExit(1)
resp = http.get("response_json") or {}
echo = resp.get("echo") or {}
if echo.get("ping") != "pong":
  print("expected echo ping pong", resp, file=sys.stderr)
  raise SystemExit(1)
PY
