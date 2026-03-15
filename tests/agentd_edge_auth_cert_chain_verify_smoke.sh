#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
ED_TOOL_BIN="${2:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "missing agentd binary path arg" >&2
  exit 2
fi
if [[ -z "${ED_TOOL_BIN}" || ! -x "${ED_TOOL_BIN}" ]]; then
  echo "missing/non-executable agent_ed25519_tool binary path arg" >&2
  exit 2
fi

CERT_TOOL="${ROOT}/tools/edge_cert_roots_tool.py"
if [[ ! -f "${CERT_TOOL}" ]]; then
  echo "missing edge_cert_roots_tool.py at ${CERT_TOOL}" >&2
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

SK_HEX="9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"
export AGENTD_RUN_ATTEST_ED25519_KID="edge-cert-ed25519-k0"
export AGENTD_RUN_ATTEST_ED25519_SEED="${SK_HEX}"

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_auth_cert_chain_verify_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

TARGET_NODE_ID="node_cert_chain_target_1"
TARGET_CAPS_SHA="sha256:a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2"
CERT_KID="lab-ca-1"
CERT_PEM_PATH="${ROOT}/tools/_compose_mtls/ca.pem"
SERVER_CERT_PATH="${ROOT}/tools/_compose_mtls/server.pem"
CLIENT_CERT_PATH="${ROOT}/tools/_compose_mtls/client.pem"
GET_JSON_PATH="${LOG_DIR}/agentd_edge_auth_cert_chain_verify.bundle.json"
OUTBOX_JSON_PATH="${LOG_DIR}/agentd_edge_auth_cert_chain_verify.outbox.json"
OUTBOX_BUNDLE_PATH="${LOG_DIR}/agentd_edge_auth_cert_chain_verify.outbox.bundle.json"

register_target_node() {
  local hello_json
  hello_json="$(python3 - <<PY
import json, uuid, time
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "NODE_HELLO",
  "from": "node:${TARGET_NODE_ID}",
  "to": "platform",
  "body": {
    "node_id": "${TARGET_NODE_ID}",
    "model": "esp32sim_cert_chain",
    "fw_git_sha": "cert-chain-f00d",
    "caps_sha256": "${TARGET_CAPS_SHA}",
  }
}))
PY
)"

  curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/json" \
    -d "${hello_json}" \
    "${DAEMON_URL}/api/v1/edge/message" >/dev/null

  local caps_rsp_json
  caps_rsp_json="$(python3 - <<PY
import json, uuid, time
manifest = {
  "spec_version": "um-acds/0.1",
  "manifest_version": "0.0.1",
  "caps_sha256": "${TARGET_CAPS_SHA}",
  "node": {"node_id": "${TARGET_NODE_ID}"},
  "runtime": {"agent_core": {"version": "0.0.0"}},
  "tools": [],
  "safety": {},
  "tags": ["edge:cert-chain-recipient"]
}
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "NODE_CAPS_RSP",
  "from": "node:${TARGET_NODE_ID}",
  "to": "platform",
  "body": {"node_id": "${TARGET_NODE_ID}", "manifest": manifest},
}))
PY
)"

  curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/json" \
    -d "${caps_rsp_json}" \
    "${DAEMON_URL}/api/v1/edge/message" >/dev/null
}

register_target_node

rotate_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json, pathlib
cert_pem = pathlib.Path("${CERT_PEM_PATH}").read_text(encoding="utf-8")
print(json.dumps({
  "mode": "replace",
  "rotation_epoch": 7,
  "edge_auth_cert_roots_pem": {
    "${CERT_KID}": cert_pem
  },
}))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/auth/cert_roots/rotate")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${rotate_resp}''')
if not obj.get("ok"):
  print("rotate not ok", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("rotation_epoch") != 7:
  print("wrong rotation epoch", obj, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/auth/cert_roots" > "${GET_JSON_PATH}"

python3 "${CERT_TOOL}" inspect \
  --bundle-json "${GET_JSON_PATH}" \
  --require-attest \
  --agent-ed25519-tool "${ED_TOOL_BIN}" > "${LOG_DIR}/agentd_edge_auth_cert_chain_verify.inspect.json"

python3 "${CERT_TOOL}" verify-cert \
  --bundle-json "${GET_JSON_PATH}" \
  --require-attest \
  --agent-ed25519-tool "${ED_TOOL_BIN}" \
  --cert "${SERVER_CERT_PATH}" > "${LOG_DIR}/agentd_edge_auth_cert_chain_verify.server.json"

python3 "${CERT_TOOL}" verify-cert \
  --bundle-json "${GET_JSON_PATH}" \
  --require-attest \
  --agent-ed25519-tool "${ED_TOOL_BIN}" \
  --cert "${CLIENT_CERT_PATH}" > "${LOG_DIR}/agentd_edge_auth_cert_chain_verify.client.json"

send_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "{\"target_node_id\":\"${TARGET_NODE_ID}\"}" \
  "${DAEMON_URL}/api/v1/edge/auth/cert_roots/send")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${send_resp}''')
if not obj.get("ok"):
  print("send not ok", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("target_node_id") != "${TARGET_NODE_ID}":
  print("wrong target", obj, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/outbox?node_id=${TARGET_NODE_ID}&cursor=0&limit=50" > "${OUTBOX_JSON_PATH}"

python3 - <<PY
import json, pathlib, sys
obj = json.loads(pathlib.Path("${OUTBOX_JSON_PATH}").read_text(encoding="utf-8"))
if not obj.get("ok"):
  print("outbox not ok", obj, file=sys.stderr)
  raise SystemExit(1)
for row in obj.get("messages") or []:
  msg = row.get("msg") or {}
  if msg.get("type") == "PLATFORM_CERT_ROOTS_BUNDLE":
    bundle = (msg.get("body") or {}).get("cert_roots")
    if not isinstance(bundle, dict):
      print("missing outbox cert_roots bundle", msg, file=sys.stderr)
      raise SystemExit(1)
    pathlib.Path("${OUTBOX_BUNDLE_PATH}").write_text(json.dumps(bundle, indent=2, sort_keys=True), encoding="utf-8")
    print("ok")
    raise SystemExit(0)
print("missing PLATFORM_CERT_ROOTS_BUNDLE", file=sys.stderr)
raise SystemExit(1)
PY

python3 "${CERT_TOOL}" inspect \
  --bundle-json "${OUTBOX_BUNDLE_PATH}" \
  --require-attest \
  --agent-ed25519-tool "${ED_TOOL_BIN}" > "${LOG_DIR}/agentd_edge_auth_cert_chain_verify.outbox.inspect.json"

python3 "${CERT_TOOL}" verify-cert \
  --bundle-json "${OUTBOX_BUNDLE_PATH}" \
  --require-attest \
  --agent-ed25519-tool "${ED_TOOL_BIN}" \
  --cert "${SERVER_CERT_PATH}" > "${LOG_DIR}/agentd_edge_auth_cert_chain_verify.outbox.server.json"

echo "agentd_edge_auth_cert_chain_verify_smoke OK"
