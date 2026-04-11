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
PORT_MODEL="$(agentd_smoke_pick_port)"
PORT_REMOTE="$(agentd_smoke_pick_port)"
PORT_PROXY="$(agentd_smoke_pick_port)"
PORT_LOCAL="$(agentd_smoke_pick_port)"
NAME="agentd_workflow_agentd_parallel_broker_routing_memory_scope_smoke"

TOKEN="token_parallel_broker_routing"
DEPLOYMENT_ID="dep-a"
export BROKER_OIDC_TOKEN="${TOKEN}"

cleanup() {
  agentd_smoke_stop

  for pid_var in PROXY_PID REMOTE_PID MODEL_PID; do
    local pid="${!pid_var:-}"
    if [[ -z "${pid}" ]]; then
      continue
    fi
    kill -TERM "${pid}" >/dev/null 2>&1 || true
    for _ in $(seq 1 30); do
      if ! kill -0 "${pid}" >/dev/null 2>&1; then
        break
      fi
      sleep 0.1
    done
    if kill -0 "${pid}" >/dev/null 2>&1; then
      kill -KILL "${pid}" >/dev/null 2>&1 || true
    fi
    wait "${pid}" >/dev/null 2>&1 || true
  done
}
trap cleanup EXIT

python3 -u - <<PY > "${LOG_DIR}/${NAME}.model.stdout.log" 2> "${LOG_DIR}/${NAME}.model.stderr.log" &
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HOST = "127.0.0.1"
PORT = int("${PORT_MODEL}")

class H(BaseHTTPRequestHandler):
  def log_message(self, fmt, *args):
    return

  def do_GET(self):
    if self.path == "/healthz":
      self.send_response(200)
      self.end_headers()
      self.wfile.write(b"ok")
      return
    self.send_response(404)
    self.end_headers()

  def do_POST(self):
    if self.path != "/v1/chat/completions":
      self.send_response(404)
      self.end_headers()
      return
    n = int(self.headers.get("Content-Length", "0") or "0")
    if n > 0:
      self.rfile.read(n)
    body = json.dumps({
      "id": "cmpl_scope",
      "object": "chat.completion",
      "choices": [
        {"index": 0, "message": {"role": "assistant", "content": "model stub ok"}, "finish_reason": "stop"}
      ],
      "usage": {"prompt_tokens": 1, "completion_tokens": 2, "total_tokens": 3}
    }).encode()
    self.send_response(200)
    self.send_header("Content-Type", "application/json")
    self.send_header("Content-Length", str(len(body)))
    self.end_headers()
    self.wfile.write(body)

ThreadingHTTPServer((HOST, PORT), H).serve_forever()
PY
MODEL_PID=$!

for _ in $(seq 1 100); do
  if curl -fsS --noproxy "*" --max-time 1 "http://${HOST}:${PORT_MODEL}/healthz" >/dev/null 2>&1; then
    break
  fi
  sleep 0.05
done
curl -fsS --noproxy "*" --max-time 2 "http://${HOST}:${PORT_MODEL}/healthz" >/dev/null
STUB_BASE="http://${HOST}:${PORT_MODEL}/v1"

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_REMOTE}" "${NAME}_remote" --tools none
REMOTE_PID="${AGENTD_PID}"
REMOTE_URL="${DAEMON_URL}"
agentd_smoke_wait_health "${REMOTE_URL}"

export REMOTE_URL
python3 -u - <<PY > "${LOG_DIR}/${NAME}.proxy.stdout.log" 2> "${LOG_DIR}/${NAME}.proxy.stderr.log" &
import os
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HOST = "127.0.0.1"
PORT = int("${PORT_PROXY}")
REMOTE = os.environ["REMOTE_URL"]
TOKEN = os.environ["BROKER_OIDC_TOKEN"]
PREFIX = "/v1/agents/agent-a/proxy"
DEPLOYMENT_ID = "${DEPLOYMENT_ID}"

def forward(method, path, query, headers, body):
  url = REMOTE + path
  if query:
    url += "?" + query
  req = urllib.request.Request(url, method=method, data=body if method in ("POST", "PUT", "PATCH") else None)
  if headers.get("content-type"):
    req.add_header("Content-Type", headers["content-type"])
  req.add_header("Accept", headers.get("accept", "application/json"))
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

  def _handle(self):
    if self.headers.get("Authorization", "") != f"Bearer {TOKEN}":
      self.send_response(401)
      self.send_header("Content-Type", "application/json")
      self.end_headers()
      self.wfile.write(b'{"ok":false,"error":"unauthorized"}')
      return
    if self.headers.get("X-Agentd-Deployment", "") != DEPLOYMENT_ID:
      self.send_response(428)
      self.send_header("Content-Type", "application/json")
      self.end_headers()
      self.wfile.write(b'{"ok":false,"error":"missing deployment routing header"}')
      return
    if not self.path.startswith(PREFIX):
      self.send_response(404)
      self.end_headers()
      return
    rest = self.path[len(PREFIX):] or "/"
    if "?" in rest:
      path, query = rest.split("?", 1)
    else:
      path, query = rest, ""
    if not path.startswith("/"):
      path = "/" + path
    body = b""
    if self.command in ("POST", "PUT", "PATCH"):
      n = int(self.headers.get("Content-Length", "0") or "0")
      if n > 0:
        body = self.rfile.read(n)
    hdrs = {k.lower(): v for k, v in self.headers.items()}
    status, rh, out = forward(self.command, path, query, hdrs, body)
    self.send_response(status)
    ctype = rh.get("Content-Type") or "application/json"
    self.send_header("Content-Type", ctype)
    self.send_header("Content-Length", str(len(out)))
    self.end_headers()
    self.wfile.write(out)

  def do_GET(self):
    self._handle()

  def do_POST(self):
    self._handle()

ThreadingHTTPServer((HOST, PORT), H).serve_forever()
PY
PROXY_PID=$!

BROKER_BASE="http://${HOST}:${PORT_PROXY}"

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_LOCAL}" "${NAME}_local" \
  --tools none \
  --workflow-enable-http-tasks \
  --workflow-http-deny-private \
  --workflow-http-allow-host "127.0.0.1:${PORT_PROXY}"
LOCAL_URL="${DAEMON_URL}"
agentd_smoke_wait_health "${LOCAL_URL}"

dup_body="${LOG_DIR}/${NAME}.duplicate_routing_body.json"
dup_status="$(curl -sS --noproxy "*" --max-time 20 \
  -o "${dup_body}" \
  -w "%{http_code}" \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
broker_proxy = {"broker_base_url": "${BROKER_BASE}", "agent_id": "agent-a", "deployment_id": "${DEPLOYMENT_ID}"}
print(json.dumps({"tasks": [{
  "task_id": "P",
  "kind": "agentd_parallel",
  "agentd_parallel": {
    "targets": [
      {"id": "one", "broker_proxy": broker_proxy},
      {"id": "two", "broker_proxy": broker_proxy},
    ],
    "routing_policy": {"require_distinct_targets": True},
    "agentd_call": {"workflow": {"tasks": [{"task_id": "W", "kind": "delay", "delay_ms": 0}]}}
  }
}]}))
PY
)" \
  "${LOCAL_URL}/api/v1/workflow/submit")"
if [[ "${dup_status}" != "400" ]]; then
  echo "expected duplicate routing policy submit to return 400, got ${dup_status}" >&2
  cat "${dup_body}" >&2
  exit 1
fi
python3 - <<PY
import json, sys
obj = json.load(open("${dup_body}"))
if "duplicate agentd_parallel routing target identity" not in obj.get("error", ""):
  print("unexpected duplicate routing body", obj, file=sys.stderr)
  raise SystemExit(1)
PY

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
tasks = [{
  "task_id": "P",
  "kind": "agentd_parallel",
  "agentd_parallel": {
    "targets": [{
      "id": "planner",
      "broker_proxy": {
        "broker_base_url": "${BROKER_BASE}",
        "agent_id": "agent-a",
        "deployment_id": "${DEPLOYMENT_ID}"
      },
      "allow_error": True
    }],
    "routing_policy": {"require_distinct_targets": True},
    "memory_scope": {"scope_id": "collab.scope", "mode": "read_only", "per_target": True},
    "agentd_call": {
      "op": "workflow_submit_and_wait",
      "bearer_env": "BROKER_OIDC_TOKEN",
      "timeout_ms": 20000,
      "poll_ms": 20,
      "max_bytes": 1048576,
      "include_tasks": True,
      "include_results": True,
      "workflow": {
        "allow_inline_api_keys": True,
        "tasks": [{
          "task_id": "W",
          "request": {
            "prompt": "scope check",
            "tools": "none",
            "base_url": "${STUB_BASE}",
            "api_key": "dummy",
            "model": "stub",
            "trace": False
          }
        }]
      }
    },
    "aggregate": {
      "mode": "first_ok",
      "ok_pointer": "/ok",
      "value_pointer": "/agentd/final/result/results_by_task/W/memory_scope_id"
    }
  }
}]
print(json.dumps({"tasks": tasks}))
PY
)" \
  "${LOCAL_URL}/api/v1/workflow/submit")"

workflow_id="$(python3 - <<PY
import json
obj = json.loads(r'''${submit_resp}''')
print(obj.get("workflow_id", ""))
PY
)"
if [[ -z "${workflow_id}" ]]; then
  echo "failed to get workflow_id: ${submit_resp}" >&2
  exit 1
fi

final=""
for _ in $(seq 1 500); do
  final="$(curl -fsS --noproxy "*" --max-time 5 \
    "${LOCAL_URL}/api/v1/workflow?workflow_id=${workflow_id}&include_tasks=1&include_results=1")"
  if python3 - <<PY >/dev/null 2>&1
import json
obj = json.loads(r'''${final}''')
st = (obj.get("workflow") or {}).get("status")
raise SystemExit(0 if st in ("done", "error", "cancelled") else 1)
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
workflow = obj.get("workflow") or {}
if workflow.get("status") != "done":
  print("expected local workflow done", workflow, file=sys.stderr)
  raise SystemExit(1)
by = (obj.get("result") or {}).get("results_by_task") or {}
p = by.get("P") or {}
attempt = by.get("P:planner") or {}
agentd = attempt.get("agentd") or {}
if p.get("ok") is not True or (p.get("assistant_text") or "") != "collab.scope:planner":
  print("unexpected aggregate result", p, file=sys.stderr)
  raise SystemExit(1)
if agentd.get("target_id") != "planner":
  print("missing target_id", agentd, file=sys.stderr)
  raise SystemExit(1)
if agentd.get("broker_agent_id") != "agent-a" or agentd.get("broker_deployment_id") != "${DEPLOYMENT_ID}":
  print("missing broker routing metadata", agentd, file=sys.stderr)
  raise SystemExit(1)
identity = agentd.get("target_identity") or ""
if not identity.startswith("broker:${BROKER_BASE}:agent-a:${DEPLOYMENT_ID}"):
  print("unexpected target_identity", identity, file=sys.stderr)
  raise SystemExit(1)
remote_result = ((agentd.get("final") or {}).get("result") or {}).get("results_by_task") or {}
w = remote_result.get("W") or {}
if w.get("memory_scope_id") != "collab.scope:planner":
  print("missing remote memory_scope_id", w, file=sys.stderr)
  raise SystemExit(1)
if w.get("memory_scope_mode") != "read_only":
  print("missing remote memory_scope_mode", w, file=sys.stderr)
  raise SystemExit(1)
if "model stub ok" not in (w.get("assistant_text") or ""):
  print("unexpected remote assistant_text", w, file=sys.stderr)
  raise SystemExit(1)
PY

echo "${NAME} OK"
