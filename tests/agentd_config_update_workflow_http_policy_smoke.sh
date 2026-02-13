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
NAME="agentd_config_update_workflow_http_policy_smoke"
TOKEN="test_token_123"

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

url="http://${HOST}:${PORT_STUB}/echo"

agentd_smoke_stop
rm -f "${DB_PATH}" >/dev/null 2>&1 || true
rm -rf "${STATE_DIR}" >/dev/null 2>&1 || true

# Start agentd with http tasks enabled and a permissive allow-host, then tighten policy at runtime via /config/update.
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}" \
  --db-path "${DB_PATH}" \
  --state-dir "${STATE_DIR}" \
  --tools none \
  --auth-token "${TOKEN}" \
  --workflow-enable-http-tasks \
  --workflow-http-allow-host "127.0.0.1:${PORT_STUB}"

agentd_smoke_wait_health "${DAEMON_URL}"

update_resp="$(curl -fsS --noproxy "*" --max-time 5 \
  -H "Authorization: Bearer ${TOKEN}" \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "workflow_http_deny_cidrs": ["127.0.0.0/8"],
  "workflow_http_deny_private_addrs": True,
  "workflow_http_dns_pin": True,
  "max_steps_default": 11,
  "max_tool_calls_total_default": 22,
  "max_tool_calls_per_tool_default": 5,
  "max_tool_call_args_chars_default": 2048,
  "tool_call_limits_default": [
    {"tool": "proc_exec", "max_calls": 1}
  ]
}))
PY
)" \
  "${DAEMON_URL}/api/v1/config/update")"

cfg_resp="$(curl -fsS --noproxy "*" --max-time 5 \
  -H "Authorization: Bearer ${TOKEN}" \
  "${DAEMON_URL}/api/v1/config")"

python3 - <<PY
import json, sys
u = json.loads(r'''${update_resp}''')
if not u.get("ok"):
  print("config/update not ok", u, file=sys.stderr)
  raise SystemExit(1)
eng = u.get("engines") or {}
if eng.get("workflow_http_dns_pin") is not True:
  print("expected update engines.workflow_http_dns_pin true", eng, file=sys.stderr)
  raise SystemExit(1)
if eng.get("workflow_http_deny_private_addrs") is not True:
  print("expected update engines.workflow_http_deny_private_addrs true", eng, file=sys.stderr)
  raise SystemExit(1)
dc = eng.get("workflow_http_deny_cidrs") or []
if "127.0.0.0/8" not in dc:
  print("expected update engines.workflow_http_deny_cidrs to include 127.0.0.0/8", eng, file=sys.stderr)
  raise SystemExit(1)
if int(u.get("max_steps_default", -1)) != 11:
  print("expected max_steps_default 11", u, file=sys.stderr)
  raise SystemExit(1)
if int(u.get("max_tool_calls_total_default", -1)) != 22:
  print("expected max_tool_calls_total_default 22", u, file=sys.stderr)
  raise SystemExit(1)
if int(u.get("max_tool_calls_per_tool_default", -1)) != 5:
  print("expected max_tool_calls_per_tool_default 5", u, file=sys.stderr)
  raise SystemExit(1)
if int(u.get("max_tool_call_args_chars_default", -1)) != 2048:
  print("expected max_tool_call_args_chars_default 2048", u, file=sys.stderr)
  raise SystemExit(1)
tl = u.get("tool_call_limits_default") or []
if not any(isinstance(x, dict) and x.get("tool") == "proc_exec" and int(x.get("max_calls", -1)) == 1 for x in tl):
  print("expected tool_call_limits_default to include proc_exec=1", tl, file=sys.stderr)
  raise SystemExit(1)

c = json.loads(r'''${cfg_resp}''')
if not c.get("ok"):
  print("config not ok", c, file=sys.stderr)
  raise SystemExit(1)
eng2 = c.get("engines") or {}
if eng2.get("workflow_http_dns_pin") is not True:
  print("expected config engines.workflow_http_dns_pin true", eng2, file=sys.stderr)
  raise SystemExit(1)
dc2 = eng2.get("workflow_http_deny_cidrs") or []
if "127.0.0.0/8" not in dc2:
  print("expected config engines.workflow_http_deny_cidrs to include 127.0.0.0/8", eng2, file=sys.stderr)
  raise SystemExit(1)
daemon = c.get("daemon") or {}
if int(daemon.get("max_steps_default", -1)) != 11:
  print("expected config daemon.max_steps_default 11", daemon, file=sys.stderr)
  raise SystemExit(1)
if int(daemon.get("max_tool_calls_total_default", -1)) != 22:
  print("expected config daemon.max_tool_calls_total_default 22", daemon, file=sys.stderr)
  raise SystemExit(1)
if int(daemon.get("max_tool_calls_per_tool_default", -1)) != 5:
  print("expected config daemon.max_tool_calls_per_tool_default 5", daemon, file=sys.stderr)
  raise SystemExit(1)
if int(daemon.get("max_tool_call_args_chars_default", -1)) != 2048:
  print("expected config daemon.max_tool_call_args_chars_default 2048", daemon, file=sys.stderr)
  raise SystemExit(1)
tl2 = daemon.get("tool_call_limits_default") or []
if not any(isinstance(x, dict) and x.get("tool") == "proc_exec" and int(x.get("max_calls", -1)) == 1 for x in tl2):
  print("expected config tool_call_limits_default to include proc_exec=1", tl2, file=sys.stderr)
  raise SystemExit(1)
PY

# Now verify the tighter policy takes effect: deny-cidr must fail closed even though allow-host permits the target.
submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Authorization: Bearer ${TOKEN}" \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
tasks = [
  {"task_id":"H","kind":"http_json","http_json":{"url":"${url}","method":"POST","timeout_ms":5000,"max_bytes":65536,"body":{"ping":"pong"}}}
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
for _ in $(seq 1 300); do
  final="$(curl -fsS --noproxy "*" --max-time 5 \
    -H "Authorization: Bearer ${TOKEN}" \
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
if w.get("status") != "error":
  print("expected workflow status error", w, file=sys.stderr)
  raise SystemExit(1)
r = (obj.get("result") or {}).get("results_by_task", {}).get("H") or {}
if r.get("ok"):
  print("expected http_json not ok", r, file=sys.stderr)
  raise SystemExit(1)
err = r.get("error","")
if "deny-cidr" not in err:
  print("expected deny-cidr error", err, file=sys.stderr)
  raise SystemExit(1)
PY

echo "${NAME} OK"
