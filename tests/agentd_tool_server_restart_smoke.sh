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
NAME="agentd_tool_server_restart_smoke"
TOOL_SERVER_LINGER_CMD="python3 -u ${SCRIPT_DIR}/tool_server_linger_on_eof.py"
TOOL_SERVER_FLAKY_CMD="python3 -u ${SCRIPT_DIR}/tool_server_flaky_exit.py"
ORPHAN_PIDS_FILE="${LOG_DIR}/${NAME}.orphan_pids"

cleanup() {
  agentd_smoke_stop
  if [[ -f "${ORPHAN_PIDS_FILE}" ]]; then
    while IFS= read -r pid; do
      [[ -n "${pid}" ]] || continue
      kill -TERM "${pid}" >/dev/null 2>&1 || true
      wait "${pid}" >/dev/null 2>&1 || true
    done < "${ORPHAN_PIDS_FILE}"
  fi
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

# Stub provider forces one tool call to server_echo for every run.
python3 -u - <<PY > "${LOG_DIR}/${NAME}.stub.stdout.log" 2> "${LOG_DIR}/${NAME}.stub.stderr.log" &
import json
from http.server import BaseHTTPRequestHandler, HTTPServer

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
                    "name": "server_echo",
                    "arguments": json.dumps({"text": "hello"}),
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

HTTPServer(("127.0.0.1", ${PORT_STUB}), H).serve_forever()
PY
STUB_PID=$!

run_once() {
  local resp
  resp="$(curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/json" \
    -d "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "Call server_echo then say OK",
  "no_session": True,
  "tools": "basic",
  "yolo": False,
  "base_url": "${STUB_BASE}",
  "api_key": "dummy",
  "model": "stub",
  "max_steps": 4,
  "verbose": True
}))
PY
)" \
    "${DAEMON_URL}/api/v1/run")"

  python3 - <<PY
import json, sys
obj = json.loads(r'''${resp}''')
if not obj.get("ok"):
  print("run failed:", obj, file=sys.stderr)
  raise SystemExit(1)
txt = (obj.get("assistant_text") or "").strip()
if txt != "OK":
  print("unexpected assistant_text:", txt, file=sys.stderr)
  raise SystemExit(1)
PY
}

record_orphan_tool_server_pid() {
  local pid
  pid="$(python3 - <<PY
import re
import subprocess

ppid = int("${AGENTD_PID}")
pattern = re.compile(r"tool_server_linger_on_eof\\.py")
out = subprocess.check_output(
    ["ps", "-ax", "-o", "pid=", "-o", "ppid=", "-o", "command="],
    text=True,
)
for line in out.splitlines():
    m = re.match(r"\\s*(\\d+)\\s+(\\d+)\\s+(.*)$", line)
    if not m:
        continue
    pid = int(m.group(1))
    parent = int(m.group(2))
    cmd = m.group(3)
    if parent != ppid or pid == ppid:
        continue
    if pattern.search(cmd):
        print(pid)
        raise SystemExit(0)
print("", end="")
PY
)"
  if [[ -z "${pid}" ]]; then
    echo "failed to locate linger tool-server child pid" >&2
    exit 1
  fi
  echo "${pid}" >> "${ORPHAN_PIDS_FILE}"
}

rm -f "${ORPHAN_PIDS_FILE}"

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}_linger" \
  --tools basic \
  --tool-server-cmd "${TOOL_SERVER_LINGER_CMD}" \
  --tool-server-timeout-ms 5000 \
  --tool-server-max-line-bytes $((1024*1024)) \
  --no-yolo

agentd_smoke_wait_health "${DAEMON_URL}"
run_once
record_orphan_tool_server_pid
kill -KILL "${AGENTD_PID}" >/dev/null 2>&1 || true
wait "${AGENTD_PID}" >/dev/null 2>&1 || true
unset AGENTD_PID

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}_linger_restart" \
  --tools basic \
  --tool-server-cmd "${TOOL_SERVER_LINGER_CMD}" \
  --tool-server-timeout-ms 5000 \
  --tool-server-max-line-bytes $((1024*1024)) \
  --no-yolo

agentd_smoke_wait_health "${DAEMON_URL}"
run_once

agentd_smoke_stop

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}" \
  --tools basic \
  --tool-server-cmd "${TOOL_SERVER_FLAKY_CMD}" \
  --tool-server-timeout-ms 5000 \
  --tool-server-max-line-bytes $((1024*1024)) \
  --no-yolo

agentd_smoke_wait_health "${DAEMON_URL}"

# Run twice: flaky server exits after first execute; agentd must restart it for the second call.
run_once
run_once

echo "${NAME} OK"
