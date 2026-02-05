#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
PLUGIN_PATH="${2:-}"
if [[ -z "${AGENTD_BIN}" || -z "${PLUGIN_PATH}" ]]; then
  echo "usage: agentd_edge_durable_workflow_submit_message_smoke.sh <agentd_bin> <plugin_path>" >&2
  exit 2
fi
if [[ ! -f "${PLUGIN_PATH}" ]]; then
  echo "plugin not found: ${PLUGIN_PATH}" >&2
  exit 2
fi

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

PORT_DAEMON="$(agentd_smoke_pick_port)"
PORT_STUB="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"
STUB_BASE="http://${HOST}:${PORT_STUB}/v1"
NAME="agentd_edge_durable_workflow_submit_message_smoke"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

# OpenAI-compatible stub: forces a single ext_echo tool call, then returns OK after tool result is present.
python3 -u - <<PY > "${LOG_DIR}/${NAME}.stub.stdout.log" 2> "${LOG_DIR}/${NAME}.stub.stderr.log" &
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

def has_tool_result(messages):
  for m in messages:
    if isinstance(m, dict) and m.get("role") == "tool":
      return True
  return False

class H(BaseHTTPRequestHandler):
  def log_message(self, fmt, *args):
    return

  def do_POST(self):
    if self.path != "/v1/chat/completions":
      self.send_response(404)
      self.end_headers()
      return
    length = int(self.headers.get("content-length") or "0")
    raw = self.rfile.read(length) if length > 0 else b"{}"
    try:
      req = json.loads(raw.decode("utf-8"))
    except Exception:
      req = {}
    messages = req.get("messages") if isinstance(req.get("messages"), list) else []

    if has_tool_result(messages):
      body = {
        "id": "cmpl_stub_2",
        "object": "chat.completion",
        "created": 0,
        "model": "stub",
        "choices": [
          {"index": 0, "message": {"role": "assistant", "content": "OK"}, "finish_reason": "stop"}
        ],
      }
    else:
      body = {
        "id": "cmpl_stub_1",
        "object": "chat.completion",
        "created": 0,
        "model": "stub",
        "choices": [
          {
            "index": 0,
            "message": {
              "role": "assistant",
              "content": "",
              "tool_calls": [
                {
                  "id": "call_1",
                  "type": "function",
                  "function": {
                    "name": "ext_echo",
                    "arguments": json.dumps({"text": "hello from stub"}),
                  },
                }
              ],
            },
            "finish_reason": "tool_calls",
          }
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

# Start agentd with daemon defaults pointing at the stub so node-submitted durable workflows can keep
# allow_inline_api_keys=false and omit api_key/base_url in the submitted task requests.
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}" \
  --tools host \
  --no-yolo \
  --tool-plugin "${PLUGIN_PATH}" \
  --base-url "${STUB_BASE}" \
  --api-key "dummy" \
  --model "stub"

agentd_smoke_wait_health "${DAEMON_URL}"

NODE_ID="node_durable_wf_msg_1"

submit_env="$(python3 - <<PY
import json, uuid, time
env = {
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "DURABLE_WORKFLOW_SUBMIT",
  "from": "node:${NODE_ID}",
  "to": "platform",
  "body": {
    "workflow": {
      "tasks": [
        {
          "task_id": "A",
          "request": {
            "prompt": "Call ext_echo then say OK",
            "no_session": True,
            "tools": "host",
            "yolo": False,
            "host_policy": "readonly",
            "max_steps": 4,
            "verbose": True,
            "trace": False
          },
          "expect": {
            "tool_called": "ext_echo",
            "tool_calls_total_between": {"min": 1, "max": 1},
            "tool_calls_for_tool_between": {"tool": "ext_echo", "min": 1, "max": 1}
          }
        }
      ]
    }
  }
}
print(json.dumps(env))
PY
)"

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "${submit_env}" \
  "${DAEMON_URL}/api/v1/edge/message")"

workflow_id="$(python3 - <<PY
import json
obj = json.loads(r'''${submit_resp}''')
print(obj.get("workflow_id","") or "")
PY
)"
if [[ -z "${workflow_id}" ]]; then
  echo "expected workflow_id in response: ${submit_resp}" >&2
  exit 1
fi

# Expect a best-effort DURABLE_WORKFLOW_ACK in outbox.
for _ in $(seq 1 60); do
  outbox="$(curl -fsS --noproxy "*" --max-time 10 \
    "${DAEMON_URL}/api/v1/edge/outbox?node_id=${NODE_ID}&cursor=0&limit=400")"
  ok="$(python3 - <<PY
import json
obj = json.loads(r'''${outbox}''')
msgs = obj.get("messages") or []
for m in msgs:
  env = m.get("msg") or {}
  if env.get("type") != "DURABLE_WORKFLOW_ACK":
    continue
  body = env.get("body") or {}
  if body.get("op") == "submit" and body.get("workflow_id") == "${workflow_id}" and body.get("ok") is True:
    print("1")
    break
else:
  print("0")
PY
)"
  if [[ "${ok}" == "1" ]]; then
    break
  fi
  sleep 0.05
done

final=""
for _ in $(seq 1 260); do
  final="$(curl -fsS --noproxy "*" --max-time 5 \
    "${DAEMON_URL}/api/v1/workflow?workflow_id=${workflow_id}&include_tasks=1&include_results=1")"
  if python3 - <<PY >/dev/null 2>&1
import json, sys
obj = json.loads(r'''${final}''')
w = obj.get("workflow") or {}
raise SystemExit(0 if w.get("status") in ("done","error","cancelled") else 1)
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
if by_id.get("A", {}).get("status") != "done":
  print("expected task A done", by_id.get("A"), file=sys.stderr)
  raise SystemExit(1)
result = obj.get("result") or {}
by_task = (result.get("results_by_task") or {})
assistant = ((by_task.get("A") or {}).get("assistant_text") or "").strip()
if assistant != "OK":
  print("expected A assistant_text OK", assistant, file=sys.stderr)
  raise SystemExit(1)
PY

# Cancel path smoke: submit a long delay workflow via DURABLE_WORKFLOW_SUBMIT, then cancel via DURABLE_WORKFLOW_CANCEL.
submit_env2="$(python3 - <<PY
import json, uuid, time
env = {
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "DURABLE_WORKFLOW_SUBMIT",
  "from": "node:${NODE_ID}",
  "to": "platform",
  "body": {
    "workflow": {
      "tasks": [
        {"task_id": "D", "kind": "delay", "delay_ms": 600000, "result": {"assistant_text": "D"}}
      ]
    }
  }
}
print(json.dumps(env))
PY
)"

submit_resp2="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "${submit_env2}" \
  "${DAEMON_URL}/api/v1/edge/message")"

workflow_id2="$(python3 - <<PY
import json
obj = json.loads(r'''${submit_resp2}''')
print(obj.get("workflow_id","") or "")
PY
)"
if [[ -z "${workflow_id2}" ]]; then
  echo "expected workflow_id in response: ${submit_resp2}" >&2
  exit 1
fi

cancel_env="$(python3 - <<PY
import json, uuid, time
env = {
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "DURABLE_WORKFLOW_CANCEL",
  "from": "node:${NODE_ID}",
  "to": "platform",
  "body": {"workflow_id": "${workflow_id2}"},
}
print(json.dumps(env))
PY
)"

curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "${cancel_env}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

# Expect a best-effort cancel ACK as well.
for _ in $(seq 1 60); do
  outbox="$(curl -fsS --noproxy "*" --max-time 10 \
    "${DAEMON_URL}/api/v1/edge/outbox?node_id=${NODE_ID}&cursor=0&limit=600")"
  ok="$(python3 - <<PY
import json
obj = json.loads(r'''${outbox}''')
msgs = obj.get("messages") or []
for m in msgs:
  env = m.get("msg") or {}
  if env.get("type") != "DURABLE_WORKFLOW_ACK":
    continue
  body = env.get("body") or {}
  if body.get("op") == "cancel" and body.get("workflow_id") == "${workflow_id2}" and body.get("ok") is True:
    print("1")
    break
else:
  print("0")
PY
)"
  if [[ "${ok}" == "1" ]]; then
    break
  fi
  sleep 0.05
done

final2=""
for _ in $(seq 1 200); do
  final2="$(curl -fsS --noproxy "*" --max-time 5 \
    "${DAEMON_URL}/api/v1/workflow?workflow_id=${workflow_id2}&include_tasks=1")"
  if python3 - <<PY >/dev/null 2>&1
import json, sys
obj = json.loads(r'''${final2}''')
w = obj.get("workflow") or {}
raise SystemExit(0 if w.get("status") in ("cancelled","done","error") else 1)
PY
  then
    break
  fi
  sleep 0.05
done

python3 - <<PY
import json, sys
obj = json.loads(r'''${final2}''')
if not obj.get("ok"):
  print("workflow get failed", obj, file=sys.stderr)
  raise SystemExit(1)
w = obj.get("workflow") or {}
if w.get("status") != "cancelled":
  print("expected workflow status cancelled", w, file=sys.stderr)
  raise SystemExit(1)
PY

