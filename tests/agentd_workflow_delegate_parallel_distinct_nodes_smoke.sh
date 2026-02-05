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
PORT_STUB_A="$(agentd_smoke_pick_port)"
PORT_STUB_B="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"
STUB_A_BASE="http://${HOST}:${PORT_STUB_A}/v1"
STUB_B_BASE="http://${HOST}:${PORT_STUB_B}/v1"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_A_PID:-}" ]]; then
    kill -TERM "${STUB_A_PID}" >/dev/null 2>&1 || true
    wait "${STUB_A_PID}" >/dev/null 2>&1 || true
  fi
  if [[ -n "${STUB_B_PID:-}" ]]; then
    kill -TERM "${STUB_B_PID}" >/dev/null 2>&1 || true
    wait "${STUB_B_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

start_stub() {
  local port="$1"
  local tag="$2"
  python3 -u - <<PY > "${LOG_DIR}/agentd_workflow_delegate_parallel_distinct_nodes_smoke.${tag}.stdout.log" 2> "${LOG_DIR}/agentd_workflow_delegate_parallel_distinct_nodes_smoke.${tag}.stderr.log" &
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

def last_user_prompt(req):
  msgs = req.get("messages") or []
  for m in reversed(msgs):
    if isinstance(m, dict) and m.get("role") == "user":
      c = m.get("content")
      if isinstance(c, str):
        return c
  return ""

class H(BaseHTTPRequestHandler):
  def log_message(self, fmt, *args):
    return

  def do_POST(self):
    if self.path != "/v1/chat/completions":
      self.send_response(404)
      self.end_headers()
      return
    raw = self.rfile.read(int(self.headers.get("Content-Length","0") or "0"))
    try:
      req = json.loads(raw.decode("utf-8"))
    except Exception:
      self.send_response(400)
      self.end_headers()
      return
    prompt = last_user_prompt(req).strip()
    body = {
      "id": "cmpl_stub",
      "object": "chat.completion",
      "created": 0,
      "model": "stub",
      "choices": [
        {"index": 0, "message": {"role": "assistant", "content": prompt}, "finish_reason": "stop"}
      ],
    }
    data = json.dumps(body).encode("utf-8")
    self.send_response(200)
    self.send_header("Content-Type", "application/json; charset=utf-8")
    self.send_header("Content-Length", str(len(data)))
    self.end_headers()
    self.wfile.write(data)

ThreadingHTTPServer(("127.0.0.1", int("${port}")), H).serve_forever()
PY
  echo $!
}

STUB_A_PID="$(start_stub "${PORT_STUB_A}" "stub_a")"
STUB_B_PID="$(start_stub "${PORT_STUB_B}" "stub_b")"

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_workflow_delegate_parallel_distinct_nodes_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(
    STUB_A_BASE="${STUB_A_BASE}" STUB_B_BASE="${STUB_B_BASE}" python3 - <<'PY'
import json, os

stub_a = os.environ["STUB_A_BASE"]
stub_b = os.environ["STUB_B_BASE"]

defaults = {"no_session": True, "tools": "none", "trace": False}
delegate = {
  "attempt_defaults": {
    "no_session": True,
    "tools": "none",
    "api_key": "dummy",
    "model": "stub",
    "trace": False
  },
  "aggregate": {
    "mode": "quorum_hashes",
    "quorum": 2,
    "require_distinct_nodes": True
    # intentionally omit pointers and node_pointer (server defaults pointers=[/assistant_text], node_pointer=/effective_base_url)
  },
  "attempts": [
    {"id": "a1", "request": {"base_url": stub_a, "prompt": "SAME"}, "expect": {"assistant_text_contains": "SAME"}},
    {"id": "a2", "request": {"base_url": stub_a, "prompt": "SAME"}, "expect": {"assistant_text_contains": "SAME"}},
    {"id": "b1", "request": {"base_url": stub_b, "prompt": "SAME"}, "expect": {"assistant_text_contains": "SAME"}},
  ]
}

tasks = [
  {"task_id": "P", "kind": "delegate_parallel", "delegate": delegate, "max_attempts": 1},
]

print(json.dumps({"tasks": tasks, "defaults": defaults, "allow_inline_api_keys": True}))
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
p = by.get("P") or {}
if p.get("kind") != "aggregate" or p.get("mode") != "quorum_hashes" or p.get("ok") is not True:
  print("expected P to be aggregate quorum_hashes ok=true", p, file=sys.stderr)
  raise SystemExit(1)
if p.get("pointers") != ["/assistant_text"]:
  print("unexpected pointers default", p.get("pointers"), file=sys.stderr)
  raise SystemExit(1)
if p.get("node_pointer") != "/effective_base_url":
  print("unexpected node_pointer default", p.get("node_pointer"), file=sys.stderr)
  raise SystemExit(1)
if p.get("require_distinct_nodes") is not True:
  print("expected require_distinct_nodes true", p.get("require_distinct_nodes"), file=sys.stderr)
  raise SystemExit(1)
checks = p.get("checks") or []
by_ptr = {c.get("ptr"): c for c in checks if isinstance(c, dict)}
c = by_ptr.get("/assistant_text") or {}
if c.get("ok") is not True:
  print("expected /assistant_text quorum ok", c, file=sys.stderr)
  raise SystemExit(1)
if c.get("count_kind") != "nodes":
  print("expected count_kind nodes", c.get("count_kind"), file=sys.stderr)
  raise SystemExit(1)
if c.get("chosen") != "SAME" or int(c.get("chosen_count", 0)) != 2:
  print("unexpected /assistant_text chosen (distinct nodes)", c, file=sys.stderr)
  raise SystemExit(1)
if (p.get("assistant_text") or "").strip() != "SAME":
  print("unexpected P assistant_text", p.get("assistant_text"), file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_workflow_delegate_parallel_distinct_nodes_smoke OK"

