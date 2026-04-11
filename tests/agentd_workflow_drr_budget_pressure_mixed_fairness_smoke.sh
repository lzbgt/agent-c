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
NAME="agentd_workflow_drr_budget_pressure_mixed_fairness_smoke"
NODE_ID="node_budget_pressure_mix"
EVENT_TYPE="budget_pressure_mix_ready"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${SENSOR_POSTER_PID:-}" ]]; then
    kill -TERM "${SENSOR_POSTER_PID}" >/dev/null 2>&1 || true
    wait "${SENSOR_POSTER_PID}" >/dev/null 2>&1 || true
  fi
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

# OpenAI-compatible stub:
# - non-streaming requests echo the last user prompt with usage
# - streaming requests emit SSE content + usage so streaming-like tasks exercise the stream path
python3 -u - <<PY > "${LOG_DIR}/${NAME}.stub.stdout.log" 2> "${LOG_DIR}/${NAME}.stub.stderr.log" &
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

def last_user_prompt(req):
  msgs = req.get("messages") if isinstance(req.get("messages"), list) else []
  for m in reversed(msgs):
    if isinstance(m, dict) and m.get("role") == "user":
      c = m.get("content")
      if isinstance(c, str):
        return c
  return ""

def usage_obj(prompt):
  if "pressure token seed" in prompt:
    return {"prompt_tokens": 1798, "completion_tokens": 2, "total_tokens": 1800}
  n = max(1, len(prompt) // 16)
  return {"prompt_tokens": n, "completion_tokens": 2, "total_tokens": n + 2}

def sse_send(handler, obj):
  data = json.dumps(obj, separators=(",", ":")).encode("utf-8")
  handler.wfile.write(b"data: " + data + b"\n\n")
  handler.wfile.flush()

def sse_done(handler):
  handler.wfile.write(b"data: [DONE]\n\n")
  handler.wfile.flush()

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
    answer = "echo:" + prompt
    usage = usage_obj(prompt)

    if req.get("stream"):
      self.send_response(200)
      self.send_header("Content-Type", "text/event-stream; charset=utf-8")
      self.send_header("Cache-Control", "no-cache")
      self.end_headers()
      sse_send(self, {"id": "cmpl_stream", "choices": [{"index": 0, "delta": {"content": answer}, "finish_reason": None}]})
      sse_send(self, {"id": "cmpl_stream", "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}], "usage": usage})
      sse_done(self)
      return

    body = {
      "id": "cmpl_stub",
      "object": "chat.completion",
      "created": 0,
      "model": "stub",
      "choices": [
        {"index": 0, "message": {"role": "assistant", "content": answer}, "finish_reason": "stop"}
      ],
      "usage": usage,
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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}" \
  --tools host \
  --host-policy full \
  --workflow-concurrency 1 \
  --workflow-poll-ms 50 \
  --workflow-max-inflight-per-workflow 1 \
  --workflow-fair-queue-policy drr \
  --workflow-drr-cost-model budget_pressure_v1

agentd_smoke_wait_health "${DAEMON_URL}"

submit_pressure="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(STUB_BASE="${STUB_BASE}" NODE_ID="${NODE_ID}" EVENT_TYPE="${EVENT_TYPE}" python3 - <<'PY'
import json, os
stub_base = os.environ["STUB_BASE"]
node_id = os.environ["NODE_ID"]
event_type = os.environ["EVENT_TYPE"]

print(json.dumps({
  "allow_sessions": True,
  "session_id": "sess_pressure",
  "session_weight": 1,
  "allow_inline_api_keys": True,
  "workflow_limits": {
    "max_total_tokens": 2000
  },
  "tasks": [
    {
      "task_id": "A_seed_tokens",
      "request": {
        "prompt": "pressure token seed",
        "no_session": True,
        "tools": "none",
        "base_url": stub_base,
        "api_key": "dummy",
        "model": "stub",
        "trace": False
      },
      "expect": {"assistant_text_contains": "pressure token seed"}
    },
    {
      "task_id": "A_gate",
      "kind": "edge_wait_sensor",
      "depends_on": ["A_seed_tokens"],
      "max_attempts": 40,
      "edge_wait_sensor": {
        "event_type": event_type,
        "node_id": node_id,
        "min_confidence": 0.8,
        "poll_ms": 25
      }
    },
    {
      "task_id": "A_host",
      "kind": "memory_put",
      "depends_on": ["A_gate"],
      "memory_put": {
        "path": "STRUCTURED.md",
        "checkpoint": True,
        "entries": [{"key": "budget_pressure_mix.host", "value": "ok"}]
      }
    },
    {
      "task_id": "A_llm",
      "depends_on": ["A_host"],
      "request": {
        "prompt": "pressure llm task",
        "no_session": True,
        "tools": "none",
        "base_url": stub_base,
        "api_key": "dummy",
        "model": "stub",
        "trace": False
      },
      "expect": {"assistant_text_contains": "pressure llm task"}
    },
    {
      "task_id": "A_stream",
      "depends_on": ["A_llm"],
      "request": {
        "prompt": "pressure streaming task",
        "no_session": True,
        "tools": "none",
        "base_url": stub_base,
        "api_key": "dummy",
        "model": "stub",
        "stream_assistant": True,
        "trace": False
      },
      "expect": {"assistant_text_contains": "pressure streaming task"}
    }
  ]
}))
PY
)" \
  "${DAEMON_URL}/api/v1/workflow/submit")"

pressure_wid="$(python3 - <<PY
import json
print(json.loads(r'''${submit_pressure}''').get("workflow_id", ""))
PY
)"
if [[ -z "${pressure_wid}" ]]; then
  echo "failed to submit pressure workflow: ${submit_pressure}" >&2
  exit 1
fi

submit_peer="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(STUB_BASE="${STUB_BASE}" NODE_ID="${NODE_ID}" EVENT_TYPE="${EVENT_TYPE}" python3 - <<'PY'
import json, os
stub_base = os.environ["STUB_BASE"]
node_id = os.environ["NODE_ID"]
event_type = os.environ["EVENT_TYPE"]
print(json.dumps({
  "allow_sessions": True,
  "session_id": "sess_peer",
  "session_weight": 1,
  "allow_inline_api_keys": True,
  "tasks": [
    {
      "task_id": "B_gate",
      "kind": "edge_wait_sensor",
      "max_attempts": 40,
      "edge_wait_sensor": {
        "event_type": event_type,
        "node_id": node_id,
        "min_confidence": 0.8,
        "poll_ms": 25
      }
    },
    {
      "task_id": "B_det",
      "kind": "delay",
      "depends_on": ["B_gate"],
      "delay_ms": 20,
      "result": {"assistant_text": "peer deterministic"}
    },
    {
      "task_id": "B_host",
      "kind": "memory_put",
      "depends_on": ["B_det"],
      "memory_put": {
        "path": "STRUCTURED.md",
        "checkpoint": True,
        "entries": [{"key": "budget_pressure_mix.peer_host", "value": "ok"}]
      }
    },
    {
      "task_id": "B_llm",
      "depends_on": ["B_host"],
      "request": {
        "prompt": "peer llm task",
        "no_session": True,
        "tools": "none",
        "base_url": stub_base,
        "api_key": "dummy",
        "model": "stub",
        "trace": False
      },
      "expect": {"assistant_text_contains": "peer llm task"}
    },
    {
      "task_id": "B_stream",
      "depends_on": ["B_llm"],
      "request": {
        "prompt": "peer streaming task",
        "no_session": True,
        "tools": "none",
        "base_url": stub_base,
        "api_key": "dummy",
        "model": "stub",
        "stream_assistant": True,
        "trace": False
      },
      "expect": {"assistant_text_contains": "peer streaming task"}
    },
    {
      "task_id": "B_tail",
      "kind": "delay",
      "depends_on": ["B_stream"],
      "delay_ms": 20,
      "result": {"assistant_text": "peer tail"}
    }
  ]
}))
PY
)" \
  "${DAEMON_URL}/api/v1/workflow/submit")"

peer_wid="$(python3 - <<PY
import json
print(json.loads(r'''${submit_peer}''').get("workflow_id", ""))
PY
)"
if [[ -z "${peer_wid}" ]]; then
  echo "failed to submit peer workflow: ${submit_peer}" >&2
  exit 1
fi

post_sensor_event() {
  local msg_id
  msg_id="$(python3 - <<'PY'
import uuid
print(str(uuid.uuid4()))
PY
)"
  local body
  body="$(NODE_ID="${NODE_ID}" EVENT_TYPE="${EVENT_TYPE}" MSG_ID="${msg_id}" python3 - <<'PY'
import json, os, time
now = int(time.time() * 1000)
print(json.dumps({
  "msg_id": os.environ["MSG_ID"],
  "ts_utc_ms": now,
  "type": "SENSOR_EVENT",
  "from": "node:" + os.environ["NODE_ID"],
  "to": "platform",
  "body": {
    "node_id": os.environ["NODE_ID"],
    "event_type": os.environ["EVENT_TYPE"],
    "ts_utc_ms": now,
    "confidence": 0.9,
    "data": {"source": "budget_pressure_mixed_fairness"}
  }
}))
PY
)"
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/json" \
    -d "${body}" \
    "${DAEMON_URL}/api/v1/edge/message" >/dev/null
}

(
  for _ in $(seq 1 800); do
    snap="$(curl -fsS --noproxy "*" --max-time 5 \
      "${DAEMON_URL}/api/v1/workflow?workflow_id=${pressure_wid}&include_tasks=1&include_results=1" 2>/dev/null || true)"
    if SNAP="${snap}" python3 - <<'PY' >/dev/null 2>&1
import json, os
try:
  obj = json.loads(os.environ.get("SNAP") or "{}")
except Exception:
  raise SystemExit(1)
tasks = obj.get("tasks") or []
by = {t.get("task_id"): t for t in tasks if isinstance(t, dict)}
t = by.get("A_gate") or {}
if int(t.get("attempt") or 0) >= 1:
  raise SystemExit(0)
raise SystemExit(1)
PY
    then
      break
    fi
    sleep 0.02
  done
  sleep 0.05
  post_sensor_event
) &
SENSOR_POSTER_PID=$!

wait_for_done() {
  local wid="${1}"
  local out_var="${2}"
  local snap=""
  for _ in $(seq 1 700); do
    snap="$(curl -fsS --noproxy "*" --max-time 5 \
      "${DAEMON_URL}/api/v1/workflow?workflow_id=${wid}&include_tasks=1&include_results=1")"
    if SNAP="${snap}" python3 - <<'PY' >/dev/null 2>&1
import json, os
obj = json.loads(os.environ.get("SNAP") or "{}")
w = obj.get("workflow") or {}
raise SystemExit(0 if w.get("status") in ("done", "error", "cancelled") else 1)
PY
    then
      printf -v "${out_var}" '%s' "${snap}"
      return 0
    fi
    sleep 0.05
  done
  echo "timed out waiting for workflow ${wid}; last=${snap}" >&2
  return 1
}

pressure_final=""
peer_final=""
wait_for_done "${pressure_wid}" pressure_final
wait_for_done "${peer_wid}" peer_final

if [[ -n "${SENSOR_POSTER_PID:-}" ]]; then
  wait "${SENSOR_POSTER_PID}" >/dev/null 2>&1 || true
  SENSOR_POSTER_PID=""
fi

PRESSURE_JSON="${pressure_final}" PEER_JSON="${peer_final}" python3 - <<'PY'
import json, os, sys

pressure = json.loads(os.environ["PRESSURE_JSON"])
peer = json.loads(os.environ["PEER_JSON"])

def workflow_done(name, obj):
  if not obj.get("ok"):
    print(f"{name} get failed: {obj}", file=sys.stderr)
    raise SystemExit(1)
  wf = obj.get("workflow") or {}
  if wf.get("status") != "done":
    print(f"{name} expected done, got {wf}", file=sys.stderr)
    raise SystemExit(1)
  return wf

workflow_done("pressure", pressure)
workflow_done("peer", peer)

def task_map(obj):
  return {t.get("task_id"): t for t in (obj.get("tasks") or []) if isinstance(t, dict)}

pt = task_map(pressure)
bt = task_map(peer)

for tid in ["A_seed_tokens", "A_gate", "A_host", "A_llm", "A_stream"]:
  if pt.get(tid, {}).get("status") != "done":
    print("pressure task not done", tid, pt.get(tid), file=sys.stderr)
    raise SystemExit(1)
for tid in ["B_gate", "B_det", "B_host", "B_llm", "B_stream", "B_tail"]:
  if bt.get(tid, {}).get("status") != "done":
    print("peer task not done", tid, bt.get(tid), file=sys.stderr)
    raise SystemExit(1)

if int(pt["A_gate"].get("attempt") or 0) < 2:
  print("expected pressure edge gate to retry before SENSOR_EVENT arrived", pt["A_gate"], file=sys.stderr)
  raise SystemExit(1)

usage = pressure.get("workflow_usage") or {}
tokens_used = int(usage.get("total_tokens_used") or 0)
if tokens_used < 1800:
  print("expected pressure workflow to consume token budget before DRR pressure check", usage, file=sys.stderr)
  raise SystemExit(1)

a_gate_finish = int(pt["A_gate"].get("finished_unix_ms") or 0)
a_host_finish = int(pt["A_host"].get("finished_unix_ms") or 0)
a_llm_start = int(pt["A_llm"].get("started_unix_ms") or 0)
a_stream_start = int(pt["A_stream"].get("started_unix_ms") or 0)
if (
  a_gate_finish <= 0 or a_host_finish <= 0 or a_llm_start <= 0 or a_stream_start <= 0 or
  not (a_gate_finish <= a_host_finish <= a_llm_start <= a_stream_start)
):
  print("invalid pressure ordering", pt["A_gate"], pt["A_host"], pt["A_llm"], pt["A_stream"], file=sys.stderr)
  raise SystemExit(1)

peer_between = []
for tid, row in bt.items():
  started = int(row.get("started_unix_ms") or 0)
  if a_gate_finish <= started <= a_stream_start:
    peer_between.append(tid)

if len(peer_between) < 2:
  print(
    "expected at least two peer tasks to run while sess_pressure carried token-budget pressure debt",
    "peer_between=", peer_between,
    "a_gate=", pt["A_gate"],
    "a_host=", pt["A_host"],
    "a_llm=", pt["A_llm"],
    "a_stream=", pt["A_stream"],
    "peer=", bt,
    file=sys.stderr,
  )
  raise SystemExit(1)

for tid in ["A_llm", "B_llm", "A_stream", "B_stream"]:
  row = (pt if tid.startswith("A_") else bt)[tid]
  res = row.get("result") or {}
  if res.get("ok") is not True:
    print("expected LLM/stream task ok", tid, res, file=sys.stderr)
    raise SystemExit(1)
  if "total_tokens" not in res:
    print("expected usage on LLM/stream task", tid, res, file=sys.stderr)
    raise SystemExit(1)

print("ok: budget_pressure_v1 mixed workload interleaved peer tasks:", peer_between)
PY

echo "${NAME} OK"
