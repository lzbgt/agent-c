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

HOST="127.0.0.1"
PORT_REMOTE="$(agentd_smoke_pick_port)"
PORT_PROXY="$(agentd_smoke_pick_port)"
PORT_LOCAL="$(agentd_smoke_pick_port)"
NAME="agentd_workflow_agentd_call_broker_proxy_smoke"

TOKEN="token_test_123"
export BROKER_OIDC_TOKEN="${TOKEN}"

cleanup() {
  # Stop local (agentd_smoke_stop uses AGENTD_PID global).
  agentd_smoke_stop

  # Stop proxy.
  if [[ -n "${PROXY_PID:-}" ]]; then
    kill -TERM "${PROXY_PID}" >/dev/null 2>&1 || true
    wait "${PROXY_PID}" >/dev/null 2>&1 || true
  fi

  # Stop remote.
  if [[ -n "${REMOTE_PID:-}" ]]; then
    kill -TERM "${REMOTE_PID}" >/dev/null 2>&1 || true
    for _ in $(seq 1 30); do
      if ! kill -0 "${REMOTE_PID}" >/dev/null 2>&1; then
        break
      fi
      sleep 0.1
    done
    if kill -0 "${REMOTE_PID}" >/dev/null 2>&1; then
      kill -KILL "${REMOTE_PID}" >/dev/null 2>&1 || true
    fi
    wait "${REMOTE_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

# Start remote agentd (the collaboration target behind the proxy).
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_REMOTE}" "${NAME}_remote" \
  --tools none
REMOTE_PID="${AGENTD_PID}"
REMOTE_URL="${DAEMON_URL}"
agentd_smoke_wait_health "${REMOTE_URL}"

# Start a tiny broker-like proxy stub that:
# - requires Authorization: Bearer $TOKEN
# - forwards /v1/agents/1/proxy/<agentd_path> to the remote agentd
python3 -u - <<PY > "${LOG_DIR}/${NAME}.proxy.stdout.log" 2> "${LOG_DIR}/${NAME}.proxy.stderr.log" &
import os
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HOST = "127.0.0.1"
PORT = int("${PORT_PROXY}")
REMOTE = os.environ.get("REMOTE_URL") or "${REMOTE_URL}"
TOKEN = os.environ.get("BROKER_OIDC_TOKEN") or ""
PREFIX = "/v1/agents/1/proxy"

def forward(method: str, path: str, query: str, headers: dict, body: bytes):
  url = REMOTE + path
  if query:
    url = url + "?" + query
  req = urllib.request.Request(url, method=method, data=body if method in ("POST","PUT","PATCH") else None)
  # Forward minimal headers (content-type); avoid forwarding hop-by-hop headers.
  ct = headers.get("content-type")
  if ct:
    req.add_header("Content-Type", ct)
  req.add_header("Accept", headers.get("accept","application/json"))
  try:
    with urllib.request.urlopen(req, timeout=10) as resp:
      out = resp.read()
      return resp.status, resp.headers, out
  except urllib.error.HTTPError as e:
    out = e.read()
    return e.code, e.headers, out

class H(BaseHTTPRequestHandler):
  def log_message(self, fmt, *args):
    return

  def _auth_ok(self) -> bool:
    want = f"Bearer {TOKEN}"
    got = self.headers.get("Authorization","")
    return bool(TOKEN) and got == want

  def _handle(self):
    if not self._auth_ok():
      self.send_response(401)
      self.send_header("Content-Type", "application/json; charset=utf-8")
      self.end_headers()
      self.wfile.write(b'{"ok":false,"error":"unauthorized"}')
      return
    if not self.path.startswith(PREFIX):
      self.send_response(404)
      self.end_headers()
      return
    rest = self.path[len(PREFIX):]
    if rest == "":
      rest = "/"
    # Split query.
    if "?" in rest:
      p, q = rest.split("?", 1)
    else:
      p, q = rest, ""
    if not p.startswith("/"):
      p = "/" + p
    body = b""
    if self.command in ("POST","PUT","PATCH"):
      n = int(self.headers.get("Content-Length","0") or "0")
      if n > 0:
        body = self.rfile.read(n)
    hdrs = {k.lower(): v for (k, v) in self.headers.items()}
    st, rh, out = forward(self.command, p, q, hdrs, body)
    self.send_response(st)
    ctype = rh.get("Content-Type") or "application/json; charset=utf-8"
    self.send_header("Content-Type", ctype)
    self.send_header("Content-Length", str(len(out)))
    self.end_headers()
    self.wfile.write(out)

  def do_GET(self): self._handle()
  def do_POST(self): self._handle()

ThreadingHTTPServer((HOST, PORT), H).serve_forever()
PY
PROXY_PID=$!
export REMOTE_URL

BROKER_BASE="http://${HOST}:${PORT_PROXY}"
PROXY_BASE="${BROKER_BASE}/v1/agents/1/proxy"

# Start local agentd that will call the remote agentd through PROXY_BASE.
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_LOCAL}" "${NAME}_local" \
  --tools none \
  --workflow-enable-http-tasks \
  --workflow-http-deny-private \
  --workflow-http-allow-host "127.0.0.1:${PORT_PROXY}"
LOCAL_URL="${DAEMON_URL}"
agentd_smoke_wait_health "${LOCAL_URL}"

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
tasks = [
  {
    "task_id": "REMOTE",
    "kind": "agentd_call",
    "agentd_call": {
      "broker_proxy": {"broker_base_url": "${BROKER_BASE}", "agent_id": "1"},
      "op": "workflow_submit_and_wait",
      "bearer_env": "BROKER_OIDC_TOKEN",
      "timeout_ms": 20000,
      "poll_ms": 20,
      "max_bytes": 1048576,
      "include_tasks": True,
      "include_results": True,
      "workflow": {
        "tasks": [
          {"task_id": "W", "kind": "delay", "delay_ms": 10, "result": {"assistant_text": "remote ok"}}
        ]
      }
    }
  }
]
print(json.dumps({"tasks": tasks}))
PY
)" \
  "${LOCAL_URL}/api/v1/workflow/submit")"

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
for _ in $(seq 1 400); do
  final="$(curl -fsS --noproxy "*" --max-time 5 \
    "${LOCAL_URL}/api/v1/workflow?workflow_id=${workflow_id}&include_tasks=1&include_results=1")"
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
if w.get("status") != "done":
  print("expected workflow status done", w, file=sys.stderr)
  raise SystemExit(1)

res = obj.get("result") or {}
by = res.get("results_by_task") or {}
r = by.get("REMOTE") or {}
if not r.get("ok"):
  print("agentd_call result not ok", r, file=sys.stderr)
  raise SystemExit(1)
agentd = r.get("agentd") or {}
if (agentd.get("base_url") or "") != "${PROXY_BASE}":
  print("expected agentd.base_url to match proxy base", agentd.get("base_url"), file=sys.stderr)
  raise SystemExit(1)
final2 = agentd.get("final") or {}
wf2 = final2.get("workflow") or {}
if wf2.get("status") != "done":
  print("expected remote workflow status done", wf2, file=sys.stderr)
  raise SystemExit(1)
r2 = (final2.get("result") or {}).get("results_by_task") or {}
wres = r2.get("W") or {}
if wres.get("assistant_text") != "remote ok":
  print("expected remote task assistant_text", wres, file=sys.stderr)
  raise SystemExit(1)
PY

echo "${NAME} OK"
