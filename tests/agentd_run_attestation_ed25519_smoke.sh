#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
ATTEST_TOOL="${2:-}"
ED_TOOL="${3:-}"
if [[ -z "${AGENTD_BIN}" || -z "${ATTEST_TOOL}" || -z "${ED_TOOL}" ]]; then
  echo "usage: agentd_run_attestation_ed25519_smoke.sh <agentd_bin> <attestation_tool> <ed25519_tool>" >&2
  exit 2
fi

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

PORT_DAEMON="$(agentd_smoke_pick_port)"
PORT_STUB="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"
STUB_BASE="http://${HOST}:${PORT_STUB}/v1"

DB_PATH="${LOG_DIR}/agentd_run_attestation_ed25519_smoke.sqlite"
SESSION_ID="agentd_run_attestation_ed25519_$(date +%s)_$RANDOM"
PROJECT_ROOT="$(agentd_smoke_project_root)"
README_PATH="${PROJECT_ROOT}/README.md"
export README_PATH

export AGENTD_RUN_ATTEST_ED25519_KID="attest_ed25519_kid_smoke"
export AGENTD_RUN_ATTEST_ED25519_SEED="9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"
unset AGENTD_RUN_ATTEST_HMAC_KID AGENTD_RUN_ATTEST_HMAC_KEY

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
  rm -f "${DB_PATH}" "${DB_PATH}-wal" "${DB_PATH}-shm" >/dev/null 2>&1 || true
}
trap cleanup EXIT

python3 -u - <<PY > "${LOG_DIR}/agentd_run_attestation_ed25519_smoke.stub.stdout.log" 2> "${LOG_DIR}/agentd_run_attestation_ed25519_smoke.stub.stderr.log" &
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
          {"index": 0, "message": {"role": "assistant", "content": None, "tool_calls": [
            {"id": "call_fs_read", "type": "function", "function": {"name": "fs_read", "arguments": json.dumps({"path": README_PATH})}}
          ]}, "finish_reason": "tool_calls"}
        ],
      }

    raw = json.dumps(body).encode("utf-8")
    self.send_response(200)
    self.send_header("content-type", "application/json")
    self.send_header("content-length", str(len(raw)))
    self.end_headers()
    self.wfile.write(raw)

server = HTTPServer(("127.0.0.1", int("${PORT_STUB}")), H)
server.serve_forever()
PY
STUB_PID=$!

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_run_attestation_ed25519_smoke" \
  --db-path "${DB_PATH}" --base-url "${STUB_BASE}"
agentd_smoke_wait_health "${DAEMON_URL}"

submit_json="$(cat <<JSON
{"session_id":"${SESSION_ID}","model":"stub","tools":"host","prompt":"hello"}
JSON
)"
resp_json="$(curl -fsS --noproxy "*" -X POST "${DAEMON_URL}/api/v1/run" -H "Content-Type: application/json" -d "${submit_json}")"

runs_json="$(curl -fsS --noproxy "*" "${DAEMON_URL}/api/v1/db/runs?limit=1&offset=0&session_id=${SESSION_ID}")"
run_id="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${runs_json}''')
runs = obj.get("runs") or []
print(runs[0].get("run_id") if runs else "")
PY
)"

if [[ -z "${run_id}" ]]; then
  echo "missing run_id" >&2
  exit 1
fi

replay_json="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/run/replay?run_id=${run_id}")"
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
if bundle.get("replay_sha256") != replay.get("replay_sha256"):
  print("replay_sha256 mismatch", bundle.get("replay_sha256"), replay.get("replay_sha256"), file=sys.stderr)
  raise SystemExit(1)
attest = bundle.get("attest") or {}
if attest.get("alg") != "ed25519":
  print("unexpected attest alg", attest.get("alg"), file=sys.stderr)
  raise SystemExit(1)
if attest.get("kid") != "attest_ed25519_kid_smoke":
  print("unexpected attest kid", attest.get("kid"), file=sys.stderr)
  raise SystemExit(1)
PY

REPLAY_PATH="${LOG_DIR}/agentd_run_attestation_ed25519_smoke.replay.json"
ATTEST_PATH="${LOG_DIR}/agentd_run_attestation_ed25519_smoke.attest.json"
export REPLAY_PATH ATTEST_PATH

python3 - <<PY
import json, os, pathlib, sys
pathlib.Path(os.environ["REPLAY_PATH"]).write_text(r'''${replay_json}''')
att_obj = json.loads(r'''${att_json}''')
bundle = att_obj.get("attestation")
if not isinstance(bundle, dict):
  print("missing attestation bundle", file=sys.stderr)
  raise SystemExit(1)
pathlib.Path(os.environ["ATTEST_PATH"]).write_text(json.dumps(bundle))
PY

PK_B64="$(${ED_TOOL} --sk-hex "${AGENTD_RUN_ATTEST_ED25519_SEED}" --print-pk-b64)"

"${ATTEST_TOOL}" --verify \
  --attestation-json "${ATTEST_PATH}" \
  --replay-json "${REPLAY_PATH}" \
  --ed25519-pubkey-b64 "${PK_B64}" >/dev/null

curl -fsS --noproxy "*" -X DELETE "${DAEMON_URL}/api/v1/session?session_id=$(python3 - <<PY
import urllib.parse
print(urllib.parse.quote("${SESSION_ID}"))
PY
)" --max-time 10 >/dev/null || true
