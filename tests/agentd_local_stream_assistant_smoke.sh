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

SESSION_ID="agentd_stream_local_$(date +%s)_$RANDOM"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

# Start an OpenAI-compatible streaming stub (SSE).
python3 -u - <<PY > "${LOG_DIR}/agentd_local_stream_assistant_smoke.stub.stdout.log" 2> "${LOG_DIR}/agentd_local_stream_assistant_smoke.stub.stderr.log" &
import json
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

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

    prompt = next((m.get("content") for m in reversed(req.get("messages", [])) if m.get("role") == "user"), "")
    case = prompt.split(":", 1)[0] if isinstance(prompt, str) else ""
    output = {
      "EMPTY_OUTPUT": {"content": ""},
      "BLANK_OUTPUT": {"content": " \t\r\n"},
      "REASONING_ONLY_OUTPUT": {"content": None, "reasoning_content": "fixture, not a reply"},
      "PADDED_OUTPUT": {"content": " \nFixture answer\t\n"},
    }.get(case)
    if not req.get("stream") or prompt == "JSON_FALLBACK_STREAM" or (output is not None and prompt.endswith(":fallback")):
      body = {
        "id": "cmpl_stub_nonstream",
        "object": "chat.completion",
        "created": 0,
        "model": "stub",
        "choices": [
          {"index": 0, "message": {"role": "assistant", **(output if output is not None else {"content": "Hello world END"})}, "finish_reason": "stop"}
        ],
      }
      data = json.dumps(body).encode("utf-8")
      self.send_response(200)
      self.send_header("Content-Type", "application/json; charset=utf-8")
      self.send_header("Content-Length", str(len(data)))
      self.end_headers()
      self.wfile.write(data)
      return

    self.send_response(200)
    self.send_header("Content-Type", "text/event-stream")
    self.send_header("Cache-Control", "no-cache")
    self.send_header("Connection", "close")
    self.end_headers()

    # Two delta chunks + DONE. Keep it minimal; client only requires data lines.
    chunks = [
      {"choices": [{"delta": {"content": "Hello "}}]},
      {"choices": [{"delta": {"content": "world END"}}]},
    ]
    if output is not None:
      chunks = [{"choices": [{"delta": output}]}]
    for c in chunks:
      line = "data: " + json.dumps(c) + "\n\n"
      self.wfile.write(line.encode("utf-8"))
      self.wfile.flush()
      time.sleep(0.05)
    if prompt == "INCOMPLETE_STREAM":
      return
    if output is not None or prompt in ("LENGTH_STREAM", "FINISH_ONLY_STREAM"):
      reason = "length" if prompt == "LENGTH_STREAM" else "stop"
      finish = {"choices": [{"delta": {}, "finish_reason": reason}]}
      self.wfile.write(("data: " + json.dumps(finish) + "\n\n").encode("utf-8"))
    if prompt != "FINISH_ONLY_STREAM":
      self.wfile.write(b"data: [DONE]\n\n")
    self.wfile.flush()

HTTPServer(("127.0.0.1", ${PORT_STUB}), H).serve_forever()
PY
STUB_PID=$!

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_local_stream_assistant_smoke" \
  --tools basic

agentd_smoke_wait_health "${DAEMON_URL}"

job_json="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "Write exactly: Hello world END",
  "session_id": "${SESSION_ID}",
  "tools": "none",
  "stream_assistant": True,
  "base_url": "${STUB_BASE}",
  "api_key": "dummy",
  "model": "stub",
  "trace": False,
  "verbose": False
}))
PY
)" \
  "${DAEMON_URL}/api/v1/run_async")"

job_id="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${job_json}''')
if not obj.get("ok") or not obj.get("job_id"):
  print("bad run_async response", obj, file=sys.stderr)
  raise SystemExit(1)
print(obj["job_id"])
PY
)"

# Stream events and ensure at least one assistant_delta appears and job_done terminates.
job_id_q="$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${job_id}")"
stream="$(curl -fsS --noproxy "*" --max-time 10 "${DAEMON_URL}/api/v1/job/stream?job_id=${job_id_q}&cursor=0" | head -n 5000)"

python3 - <<PY
import re, sys
s = r'''${stream}'''
has_delta = False
has_done = ("event: job_done" in s)
for m in re.finditer(r"^data: (.*)$", s, flags=re.M):
  line = m.group(1)
  if '"type":"assistant_delta"' in line or '"type": "assistant_delta"' in line:
    has_delta = True
if not has_done:
  print("missing job_done in SSE stream", file=sys.stderr)
  raise SystemExit(1)
if not has_delta:
  print("expected at least one assistant_delta event", file=sys.stderr)
  raise SystemExit(1)
PY

final="$(curl -fsS --noproxy "*" --max-time 10 "${DAEMON_URL}/api/v1/job?job_id=${job_id_q}")"

python3 - <<PY
import json, sys
obj=json.loads(r'''${final}''')
if not obj.get("ok") or obj.get("status") != "done":
  print("job did not complete", obj, file=sys.stderr)
  raise SystemExit(1)
res=obj.get("result") or {}
if not isinstance(res, dict) or not res.get("ok"):
  print("run result not ok", obj, file=sys.stderr)
  raise SystemExit(1)
txt=(res.get("assistant_text") or "").strip()
if txt != "Hello world END":
  print(f"unexpected assistant_text: {txt!r}", file=sys.stderr)
  raise SystemExit(1)
PY

# Ensure streamed runs persist assistant messages to the session.
sid_q="$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${SESSION_ID}")"
sess="$(curl -fsS --noproxy "*" --max-time 10 "${DAEMON_URL}/api/v1/session?session_id=${sid_q}")"

python3 - <<PY
import json, sys
obj=json.loads(r'''${sess}''')
if not obj.get("ok"):
  print("session endpoint failed", obj, file=sys.stderr)
  raise SystemExit(1)
msgs=obj.get("messages") or []
if not isinstance(msgs, list) or len(msgs) < 2:
  print("expected session to have at least user+assistant messages", obj, file=sys.stderr)
  raise SystemExit(1)
last=msgs[-1]
if not isinstance(last, dict) or last.get("role") != "assistant":
  print("expected last message role=assistant", last, file=sys.stderr)
  raise SystemExit(1)
if (last.get("content") or "").strip() != "Hello world END":
  print("expected last assistant content persisted", last, file=sys.stderr)
  raise SystemExit(1)
PY

# The daemon must not commit an assistant message from a partial preview.
python3 - "${DAEMON_URL}" "${STUB_BASE}" "${SESSION_ID}" <<'PY'
import json, sys, urllib.error, urllib.request, urllib.parse
daemon, stub, sid = sys.argv[1:]
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

def call(path, request=None):
    options = {}
    if request is not None:
        options = {"data": json.dumps(request).encode(), "headers": {"Content-Type": "application/json"}}
    target = urllib.request.Request(daemon + path, **options)
    try:
        response = opener.open(target, timeout=10)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        return json.load(response)

for prompt, should_complete in (("INCOMPLETE_STREAM", False), ("LENGTH_STREAM", False),
                                ("FINISH_ONLY_STREAM", True), ("JSON_FALLBACK_STREAM", True)):
    session = sid + "_" + prompt.lower()
    result = call("/api/v1/run", {"prompt": prompt, "session_id": session,
                  "tools": "none", "stream_assistant": True, "base_url": stub,
                  "api_key": "dummy", "model": "stub", "trace": False, "verbose": False})
    assert result.get("ok") is should_complete, (prompt, result)
    if should_complete:
        assert result.get("assistant_text") == "Hello world END", (prompt, result)
    else:
        assert not result.get("assistant_text"), (prompt, result)
        if prompt == "INCOMPLETE_STREAM":
            assert "completion marker" in json.dumps(result.get("error")), result
        stored = call("/api/v1/session?" + urllib.parse.urlencode({"session_id": session}))
        assert not any(m.get("role") == "assistant" for m in stored.get("messages", [])), stored

for tools in ("none", "basic"):
    for mode in ("raw", "sse", "fallback"):
        for index, prompt in enumerate(("EMPTY_OUTPUT", "BLANK_OUTPUT", "REASONING_ONLY_OUTPUT", "PADDED_OUTPUT")):
            session = f"{sid}_{tools}_{mode}_{index}"
            expected = " \nFixture answer\t\n" if prompt == "PADDED_OUTPUT" else None
            suffix = ":fallback" if mode == "fallback" else ""
            result = call("/api/v1/run", {"prompt": prompt + suffix, "session_id": session,
                          "tools": tools, "stream_assistant": mode != "raw", "base_url": stub,
                          "api_key": "dummy", "model": "stub", "trace": False, "verbose": False})
            stored = call("/api/v1/session?" + urllib.parse.urlencode({"session_id": session}))
            replies = [m["content"] for m in stored.get("messages", []) if m.get("role") == "assistant"]
            assert result.get("ok") is (expected is not None), (tools, mode, prompt, result)
            if expected is None:
                assert "without an assistant answer" in json.dumps(result.get("error")), result
                assert not result.get("assistant_text") and not replies, (result, stored)
            else:
                assert result.get("assistant_text") == expected and replies == [expected], (result, stored)
PY

# Cleanup session artifacts.
curl -fsS --noproxy "*" -X DELETE "${DAEMON_URL}/api/v1/session?session_id=${sid_q}" --max-time 10 >/dev/null || true
curl -fsS --noproxy "*" -X DELETE "${DAEMON_URL}/api/v1/job?job_id=${job_id_q}" --max-time 5 >/dev/null || true

