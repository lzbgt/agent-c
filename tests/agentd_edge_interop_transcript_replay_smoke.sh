#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
FIXTURE_PATH="${2:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "usage: agentd_edge_interop_transcript_replay_smoke.sh <agentd_bin> [fixture_jsonl]" >&2
  exit 2
fi

if [[ -z "${FIXTURE_PATH}" ]]; then
  ROOT="$(agentd_smoke_project_root)"
  FIXTURE_PATH="${ROOT}/docs/spec/um-eais/fixtures/umbmp_workflow_submit_cancel_v0.1.jsonl"
fi
if [[ ! -f "${FIXTURE_PATH}" ]]; then
  echo "fixture not found: ${FIXTURE_PATH}" >&2
  exit 2
fi

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

PORT_DAEMON="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_interop_transcript_replay_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

node_id="$(python3 - <<PY
import json
path = r'''${FIXTURE_PATH}'''
node = ""
for line in open(path, "r", encoding="utf-8"):
  line = line.strip()
  if not line:
    continue
  env = json.loads(line)
  if env.get("type") == "NODE_HELLO":
    node = (env.get("body") or {}).get("node_id") or ""
    break
print(node)
PY
)"

workflow_id="$(python3 - <<PY
import json
path = r'''${FIXTURE_PATH}'''
wid = ""
for line in open(path, "r", encoding="utf-8"):
  line = line.strip()
  if not line:
    continue
  env = json.loads(line)
  if env.get("type") == "WORKFLOW_SUBMIT":
    body = env.get("body") or {}
    wf = body.get("workflow") if isinstance(body.get("workflow"), dict) else body
    if isinstance(wf, dict):
      wid = wf.get("workflow_id") or ""
    break
print(wid)
PY
)"

if [[ -z "${node_id}" || -z "${workflow_id}" ]]; then
  echo "failed to extract node_id/workflow_id from fixture: ${FIXTURE_PATH}" >&2
  exit 1
fi

while IFS= read -r line; do
  # Skip blank lines (fixtures may include trailing newline).
  if [[ -z "${line//[[:space:]]/}" ]]; then
    continue
  fi
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/json" \
    -d "${line}" \
    "${DAEMON_URL}/api/v1/edge/message" >/dev/null
done < "${FIXTURE_PATH}"

# Expect both a submit ACK and a cancel ACK in outbox.
for _ in $(seq 1 100); do
  outbox="$(curl -fsS --noproxy "*" --max-time 10 \
    "${DAEMON_URL}/api/v1/edge/outbox?node_id=${node_id}&cursor=0&limit=800")"
  ok="$(python3 - <<PY
import json
obj = json.loads(r'''${outbox}''')
msgs = obj.get("messages") or []
submit_ok = False
cancel_ok = False
for m in msgs:
  env = m.get("msg") or {}
  if env.get("type") != "WORKFLOW_ACK":
    continue
  body = env.get("body") or {}
  if body.get("workflow_id") != "${workflow_id}":
    continue
  if body.get("ok") is True and body.get("status") == "CANCELED":
    cancel_ok = True
  if body.get("ok") is True and ("status" not in body):
    submit_ok = True
print("1" if (submit_ok and cancel_ok) else "0")
PY
)"
  if [[ "${ok}" == "1" ]]; then
    break
  fi
  sleep 0.05
done

final=""
for _ in $(seq 1 160); do
  final="$(curl -fsS --noproxy "*" --max-time 10 \
    "${DAEMON_URL}/api/v1/edge/workflow?workflow_id=${workflow_id}&include_steps=1")"
  if python3 - <<PY >/dev/null 2>&1
import json, sys
obj = json.loads(r'''${final}''')
wf = obj.get("workflow") or {}
raise SystemExit(0 if wf.get("status") == "CANCELED" else 1)
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
wf = obj.get("workflow") or {}
if wf.get("status") != "CANCELED":
  print("expected workflow status CANCELED", wf, file=sys.stderr)
  raise SystemExit(1)
PY

