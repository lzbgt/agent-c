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
HOST="127.0.0.1"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_caps_sha256_validation_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

NODE_ID="node_caps_sha_bad_1"

hello_bad="$(python3 - <<PY
import json, uuid, time
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "NODE_HELLO",
  "from": f"node:{'${NODE_ID}'}",
  "to": "platform",
  "body": {
    "node_id": "${NODE_ID}",
    "caps_sha256": "sha256:not_hex",
  }
}))
PY
)"

status_hello="$(curl -sS --noproxy "*" --max-time 10 \
  -o "${LOG_DIR}/agentd_edge_caps_sha256_validation_smoke.hello_bad.body.json" \
  -w "%{http_code}" \
  -H "Content-Type: application/json" \
  -d "${hello_bad}" \
  "${DAEMON_URL}/api/v1/edge/message")"

if [[ "${status_hello}" != "400" ]]; then
  echo "expected NODE_HELLO with invalid caps_sha256 to return 400, got ${status_hello}" >&2
  cat "${LOG_DIR}/agentd_edge_caps_sha256_validation_smoke.hello_bad.body.json" >&2 || true
  exit 1
fi

caps_rsp_bad="$(python3 - <<PY
import json, uuid, time
manifest = {
  "spec_version": "um-acds/0.1",
  "manifest_version": "0.0.1",
  "caps_sha256": "sha256:not_hex",
  "node": {"node_id": "${NODE_ID}"},
  "runtime": {"agent_core": {"version": "0.0.0"}},
  "hardware": {"presence": {}},
  "tools": [],
  "safety": {},
}
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "NODE_CAPS_RSP",
  "from": f"node:{'${NODE_ID}'}",
  "to": "platform",
  "body": {"node_id": "${NODE_ID}", "manifest": manifest},
}))
PY
)"

status_caps="$(curl -sS --noproxy "*" --max-time 10 \
  -o "${LOG_DIR}/agentd_edge_caps_sha256_validation_smoke.caps_bad.body.json" \
  -w "%{http_code}" \
  -H "Content-Type: application/json" \
  -d "${caps_rsp_bad}" \
  "${DAEMON_URL}/api/v1/edge/message")"

if [[ "${status_caps}" != "400" ]]; then
  echo "expected NODE_CAPS_RSP with invalid manifest.caps_sha256 to return 400, got ${status_caps}" >&2
  cat "${LOG_DIR}/agentd_edge_caps_sha256_validation_smoke.caps_bad.body.json" >&2 || true
  exit 1
fi

echo "agentd_edge_caps_sha256_validation_smoke OK"

