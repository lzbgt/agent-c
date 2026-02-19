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

DB_PATH="${LOG_DIR}/agentd_run_replay_smoke.sqlite"
SESSION_ID="agentd_run_replay_$(date +%s)_$RANDOM"
PROJECT_ROOT="$(agentd_smoke_project_root)"
README_PATH="${PROJECT_ROOT}/README.md"
export README_PATH
export AGENTD_RUN_ATTEST_HMAC_KID="attest_kid_smoke"
export AGENTD_RUN_ATTEST_HMAC_KEY="attest_secret_smoke"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
  rm -f "${DB_PATH}" "${DB_PATH}-wal" "${DB_PATH}-shm" >/dev/null 2>&1 || true
}
trap cleanup EXIT

# OpenAI-compatible stub:
# - first response: tool call to fs_read README.md
# - second response (after tool result): assistant "OK"
python3 -u - <<PY > "${LOG_DIR}/agentd_run_replay_smoke.stub.stdout.log" 2> "${LOG_DIR}/agentd_run_replay_smoke.stub.stderr.log" &
import json
import os
from http.server import BaseHTTPRequestHandler, HTTPServer

README_PATH = os.environ.get("README_PATH", "README.md")

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
                    "name": "fs_read",
                    "arguments": json.dumps({"path": README_PATH, "max_lines": 5, "max_chars": 20000}),
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

rm -f "${DB_PATH}" "${DB_PATH}-wal" "${DB_PATH}-shm" >/dev/null 2>&1 || true

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_run_replay_smoke" \
  --tools host \
  --no-yolo \
  --db-path "${DB_PATH}"

agentd_smoke_wait_health "${DAEMON_URL}"

resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "prompt": "read the first 5 lines of README.md and say OK",
  "session_id": "${SESSION_ID}",
  "tools": "host",
  "base_url": "${STUB_BASE}",
  "model": "stub",
  "api_key": "secret_should_not_persist"
}))
PY
)" \
  "${DAEMON_URL}/api/v1/run")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp}''')
if not obj.get("ok"):
  print("run failed", obj, file=sys.stderr)
  raise SystemExit(1)
PY

runs_json="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/runs?session_id=$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote("${SESSION_ID}"))
PY
)")"

run_id="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${runs_json}''')
if not obj.get("ok"):
  print("db runs failed", obj, file=sys.stderr)
  raise SystemExit(1)
runs = obj.get("runs") or []
if not runs:
  print("no runs", file=sys.stderr)
  raise SystemExit(1)
print(runs[0].get("run_id") or "")
PY
)"

if [[ -z "${run_id}" ]]; then
  echo "missing run_id" >&2
  exit 1
fi

replay_json="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/run/replay?run_id=${run_id}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${replay_json}''')
if not obj.get("ok"):
  print("replay failed", obj, file=sys.stderr)
  raise SystemExit(1)
bundle = obj.get("bundle") or {}
if bundle.get("schema") != "run_replay_bundle_v1":
  print("unexpected bundle schema", bundle.get("schema"), file=sys.stderr)
  raise SystemExit(1)
req = bundle.get("request") or {}
if "api_key" in req:
  print("api_key should be redacted", file=sys.stderr)
  raise SystemExit(1)
resp = bundle.get("response") or {}
if resp.get("assistant_text") != "OK":
  print("unexpected assistant_text", resp.get("assistant_text"), file=sys.stderr)
  raise SystemExit(1)
trs = bundle.get("tool_records") or []
if not any(tr.get("tool_name") == "fs_read" for tr in trs):
  print("missing fs_read tool record", trs, file=sys.stderr)
  raise SystemExit(1)
sha = obj.get("replay_sha256") or ""
if not (isinstance(sha, str) and sha.startswith("sha256:")):
  print("missing replay_sha256", sha, file=sys.stderr)
  raise SystemExit(1)
if obj.get("replay_sha256_alg") != "agent_json_c14n_v1":
  print("unexpected replay_sha256_alg", obj.get("replay_sha256_alg"), file=sys.stderr)
  raise SystemExit(1)
if obj.get("replay_sha256_schema") != "run_replay_bundle_v1":
  print("unexpected replay_sha256_schema", obj.get("replay_sha256_schema"), file=sys.stderr)
  raise SystemExit(1)
PY

att_json="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/run/attestation?run_id=${run_id}")"

python3 - <<PY
import json, sys
replay = json.loads(r'''${replay_json}''')
att = json.loads(r'''${att_json}''')
if not att.get("ok"):
  print("attestation failed", att, file=sys.stderr)
  raise SystemExit(1)
bundle = att.get("attestation") or {}
if bundle.get("schema") != "run_attestation_bundle_v1":
  print("unexpected attestation schema", bundle.get("schema"), file=sys.stderr)
  raise SystemExit(1)
if not isinstance(bundle.get("created_utc_ms"), int) or bundle.get("created_utc_ms", 0) <= 0:
  print("missing created_utc_ms", bundle.get("created_utc_ms"), file=sys.stderr)
  raise SystemExit(1)
if bundle.get("replay_sha256") != replay.get("replay_sha256"):
  print("replay_sha256 mismatch", bundle.get("replay_sha256"), replay.get("replay_sha256"), file=sys.stderr)
  raise SystemExit(1)
if bundle.get("replay_sha256_alg") != "agent_json_c14n_v1":
  print("unexpected replay_sha256_alg", bundle.get("replay_sha256_alg"), file=sys.stderr)
  raise SystemExit(1)
if bundle.get("replay_sha256_schema") != "run_replay_bundle_v1":
  print("unexpected replay_sha256_schema", bundle.get("replay_sha256_schema"), file=sys.stderr)
  raise SystemExit(1)
attest = bundle.get("attest") or {}
if not attest:
  print("missing attest block", attest, file=sys.stderr)
  raise SystemExit(1)
if attest.get("alg") != "hmac-sha256":
  print("unexpected attest alg", attest, file=sys.stderr)
  raise SystemExit(1)
if attest.get("kid") != "attest_kid_smoke":
  print("unexpected attest kid", attest.get("kid"), file=sys.stderr)
  raise SystemExit(1)
if not isinstance(attest.get("sig"), str) or not attest.get("sig"):
  print("missing attest sig", attest, file=sys.stderr)
  raise SystemExit(1)
if attest.get("signing_schema") != "run_attestation_bundle_v1":
  print("unexpected signing_schema", attest.get("signing_schema"), file=sys.stderr)
  raise SystemExit(1)
PY

# Cleanup session artifacts.
curl -fsS --noproxy "*" -X DELETE "${DAEMON_URL}/api/v1/session?session_id=$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote("${SESSION_ID}"))
PY
)" --max-time 10 >/dev/null || true
