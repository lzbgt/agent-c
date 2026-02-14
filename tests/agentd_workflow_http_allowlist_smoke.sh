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

PORT_STUB="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"
NAME="agentd_workflow_http_allowlist_smoke"

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

url="http://${HOST}:${PORT_STUB}/echo"

run_one() {
  local allow_host="$1"
  local allow_cidr="$2"
  local deny_private="$3"
  local expect_status="$4" # done|error

  agentd_smoke_stop
  local port_daemon
  port_daemon="$(agentd_smoke_pick_port)"
  local db_path="${LOG_DIR}/${NAME}_${expect_status}_${port_daemon}.sqlite"
  local state_dir="${LOG_DIR}/${NAME}_${expect_status}_${port_daemon}.state"

  local extra=()
  if [[ -n "${allow_host}" ]]; then
    extra+=(--workflow-http-allow-host "${allow_host}")
  fi
  if [[ -n "${allow_cidr}" ]]; then
    extra+=(--workflow-http-allow-cidr "${allow_cidr}")
  fi
  if [[ "${deny_private}" == "1" ]]; then
    extra+=(--workflow-http-deny-private)
  fi

  agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${port_daemon}" "${NAME}_${expect_status}" \
    --db-path "${db_path}" \
    --state-dir "${state_dir}" \
    --tools none \
    --workflow-enable-http-tasks \
    "${extra[@]}"

  agentd_smoke_wait_health "${DAEMON_URL}"

  submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
    -H "Content-Type: application/json" \
    -d "$(python3 - <<PY
import json
tasks = [
  {
    "task_id": "H",
    "kind": "http_json",
    "http_json": {
      "url": "${url}",
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
    return 1
  fi

  final=""
  for _ in $(seq 1 300); do
    final="$(curl -fsS --noproxy "*" --max-time 5 \
      "${DAEMON_URL}/api/v1/workflow?workflow_id=${workflow_id}&include_tasks=1&include_results=1")"
    if python3 - <<PY >/dev/null 2>&1
import json
obj = json.loads(r'''${final}''')
w = obj.get("workflow") or {}
st = w.get("status")
raise SystemExit(0 if st in ("done","error","cancelled") else 1)
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
st = w.get("status")
if st != "${expect_status}":
  print("expected status ${expect_status}", w, file=sys.stderr)
  raise SystemExit(1)
tasks = obj.get("tasks") or []
by_id = {t.get("task_id"): t for t in tasks if isinstance(t, dict)}
if by_id.get("H", {}).get("status") != st:
  # In error case, task is error; in done case, task is done.
  print("unexpected task status", by_id.get("H"), file=sys.stderr)
  raise SystemExit(1)
res = obj.get("result") or {}
by = res.get("results_by_task") or {}
r = by.get("H") or {}
if st == "done":
  if not r.get("ok"):
    print("expected ok result", r, file=sys.stderr)
    raise SystemExit(1)
  http = r.get("http") or {}
  if http.get("status") != 200:
    print("expected 200", http, file=sys.stderr)
    raise SystemExit(1)
else:
  if r.get("ok"):
    print("expected not ok result", r, file=sys.stderr)
    raise SystemExit(1)
  err = r.get("error","")
  if "allowlist" not in err and "not allowed" not in err and "outbound policy" not in err:
    print("expected allowlist error", err, file=sys.stderr)
    raise SystemExit(1)
PY
}

# Deny: allowlist does not include 127.0.0.1 so the deterministic http task must fail closed.
run_one "example.com" "" "0" "error"

# Deny-private with no allowlist: 127.0.0.1 target should be rejected (defense-in-depth).
run_one "" "" "1" "error"

# Allow: allowlist includes the stub host; request should succeed.
run_one "127.0.0.1" "" "0" "done"

# Allow via CIDR even with deny-private enabled.
run_one "" "127.0.0.0/8" "1" "done"

# Allow-host can explicitly permit a literal IP even with deny-private enabled.
run_one "127.0.0.1" "" "1" "done"
