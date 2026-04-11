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

DB_PATH="${LOG_DIR}/agentd_workflow_replay_spec_smoke_${PORT_DAEMON}.sqlite"
STATE_DIR="${LOG_DIR}/agentd_workflow_replay_spec_smoke_${PORT_DAEMON}.state"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

# OpenAI-compatible deterministic stub: echoes the last user prompt.
python3 -u - <<PY > "${LOG_DIR}/agentd_workflow_replay_spec_smoke.stub.stdout.log" 2> "${LOG_DIR}/agentd_workflow_replay_spec_smoke.stub.stderr.log" &
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
    raw = self.rfile.read(int(self.headers.get("Content-Length", "0") or "0"))
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

ThreadingHTTPServer(("127.0.0.1", ${PORT_STUB}), H).serve_forever()
PY
STUB_PID=$!

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_workflow_replay_spec_smoke" \
  --db-path "${DB_PATH}" \
  --state-dir "${STATE_DIR}" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

wait_workflow() {
  local workflow_id="$1"
  local final=""
  for _ in $(seq 1 160); do
    final="$(curl -fsS --noproxy "*" --max-time 5 \
      "${DAEMON_URL}/api/v1/workflow?workflow_id=${workflow_id}&include_tasks=1&include_results=1&include_spec=1")"
    if FINAL="${final}" python3 - <<'PY' >/dev/null 2>&1
import json, os
obj = json.loads(os.environ["FINAL"])
st = (obj.get("workflow") or {}).get("status")
raise SystemExit(0 if st in ("done", "error", "cancelled") else 1)
PY
    then
      printf '%s\n' "${final}"
      return 0
    fi
    sleep 0.05
  done
  echo "workflow did not reach terminal status: ${workflow_id}" >&2
  return 1
}

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
req_a = {
  "prompt": "Alpha",
  "no_session": True,
  "tools": "none",
  "base_url": "${STUB_BASE}",
  "api_key": "dummy",
  "model": "stub",
  "trace": False,
}
req_b = {
  "prompt": "B got \${task.A.json:/assistant_text}",
  "no_session": True,
  "tools": "none",
  "base_url": "${STUB_BASE}",
  "api_key": "dummy",
  "model": "stub",
  "trace": False,
}
tasks = [
  {
    "task_id": "A",
    "request": req_a,
    "expect": {
      "json_pointer_schema": {"pointer": "/assistant_text", "schema": {"type": "string", "enum": ["Alpha"]}}
    },
  },
  {
    "task_id": "B",
    "depends_on": ["A"],
    "request": req_b,
    "expect": {
      "assistant_text_contains": "B got Alpha",
      "json_pointer_regex": {"pointer": "/assistant_text", "regex": "^B got Alpha$"},
    },
  },
]
print(json.dumps({
  "workflow_id": "wf_replay_original_${PORT_DAEMON}",
  "trace_id": "trace_replay_original_${PORT_DAEMON}",
  "idempotency_key": "idem_replay_original_${PORT_DAEMON}",
  "tasks": tasks,
  "allow_inline_api_keys": True,
}))
PY
)" \
  "${DAEMON_URL}/api/v1/workflow/submit")"

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

original_final="$(wait_workflow "${workflow_id}")"

replay_submit_body="$(ORIGINAL_FINAL="${original_final}" PORT_DAEMON="${PORT_DAEMON}" python3 - <<'PY'
import json, os, sys
obj = json.loads(os.environ["ORIGINAL_FINAL"])
spec = obj.get("spec")
if not isinstance(spec, dict):
  print("missing persisted workflow spec", file=sys.stderr)
  raise SystemExit(1)
spec_json = obj.get("spec_json") or ""
if "***redacted***" not in spec_json:
  print("expected redacted api_key in persisted spec", file=sys.stderr)
  raise SystemExit(1)
port = os.environ["PORT_DAEMON"]
spec["workflow_id"] = f"wf_replay_resubmitted_{port}"
spec["trace_id"] = f"trace_replay_resubmitted_{port}"
spec["idempotency_key"] = f"idem_replay_resubmitted_{port}"
spec.pop("submitted_unix_ms", None)
print(json.dumps(spec))
PY
)"

replay_submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "${replay_submit_body}" \
  "${DAEMON_URL}/api/v1/workflow/submit")"

replay_workflow_id="$(python3 - <<PY
import json
obj = json.loads(r'''${replay_submit_resp}''')
print(obj.get("workflow_id", ""))
PY
)"
if [[ -z "${replay_workflow_id}" ]]; then
  echo "failed to get replay workflow_id: ${replay_submit_resp}" >&2
  exit 1
fi

replay_final="$(wait_workflow "${replay_workflow_id}")"

ORIGINAL_FINAL="${original_final}" REPLAY_FINAL="${replay_final}" python3 - <<'PY'
import json, os, sys

orig = json.loads(os.environ["ORIGINAL_FINAL"])
replay = json.loads(os.environ["REPLAY_FINAL"])

def task_texts(doc):
  if not doc.get("ok"):
    raise AssertionError(f"workflow get failed: {doc}")
  wf = doc.get("workflow") or {}
  if wf.get("status") != "done":
    raise AssertionError(f"workflow not done: {wf}")
  tasks = doc.get("tasks") or []
  statuses = {t.get("task_id"): t.get("status") for t in tasks if isinstance(t, dict)}
  if statuses != {"A": "done", "B": "done"}:
    raise AssertionError(f"unexpected task statuses: {statuses}")
  by_task = ((doc.get("result") or {}).get("results_by_task") or {})
  return {tid: (by_task.get(tid) or {}).get("assistant_text") for tid in ("A", "B")}

orig_texts = task_texts(orig)
replay_texts = task_texts(replay)
if orig_texts != replay_texts:
  print("workflow replay output mismatch", orig_texts, replay_texts, file=sys.stderr)
  raise SystemExit(1)
if replay_texts != {"A": "Alpha", "B": "B got Alpha"}:
  print("unexpected replay texts", replay_texts, file=sys.stderr)
  raise SystemExit(1)
orig_spec = orig.get("spec") or {}
replay_spec = replay.get("spec") or {}
for spec in (orig_spec, replay_spec):
  if not isinstance(spec.get("tasks"), list) or len(spec["tasks"]) != 2:
    print("expected persisted replayable tasks", spec, file=sys.stderr)
    raise SystemExit(1)
if orig_spec.get("workflow_id") == replay_spec.get("workflow_id"):
  print("expected replay to use a fresh workflow_id", file=sys.stderr)
  raise SystemExit(1)
if (replay_spec.get("tasks") or [])[0].get("request", {}).get("api_key") != "***redacted***":
  print("expected replayed persisted spec to use redacted api_key", replay_spec, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_workflow_replay_spec_smoke OK"
