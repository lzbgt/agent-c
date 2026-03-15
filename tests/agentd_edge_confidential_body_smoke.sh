#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
CONF_TOOL_BIN="${2:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "missing agentd binary path arg" >&2
  exit 2
fi
if [[ -z "${CONF_TOOL_BIN}" ]]; then
  echo "missing edge_confidentiality_tool binary path arg" >&2
  exit 2
fi

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

PORT_DAEMON="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"
NODE_ID="node_confidential_1"
CAPS_SHA="sha256:cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd"
CONF_KID="node-confidential:v1"
CONF_SECRET="node-confidential-secret"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_confidential_body_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "edge_confidentiality_required": True,
  "edge_confidentiality_keys": {
    "${CONF_KID}": "${CONF_SECRET}"
  }
}))
PY
)" \
  "${DAEMON_URL}/api/v1/config/update" >/dev/null

plain_hello_json="$(python3 - <<PY
import json, time, uuid
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "NODE_HELLO",
  "from": "node:${NODE_ID}",
  "to": "platform",
  "body": {
    "node_id": "${NODE_ID}",
    "model": "esp32sim_confidential",
    "fw_git_sha": "plainreject",
    "caps_sha256": "${CAPS_SHA}",
  }
}))
PY
)"

plain_status="$(curl -sS --noproxy "*" --max-time 10 -o "${LOG_DIR}/agentd_edge_confidential_body_plain.json" -w "%{http_code}" \
  -H "Content-Type: application/json" \
  -d "${plain_hello_json}" \
  "${DAEMON_URL}/api/v1/edge/message")"
[[ "${plain_status}" == "401" ]] || {
  echo "expected 401 for plaintext body under confidentiality_required, got ${plain_status}" >&2
  cat "${LOG_DIR}/agentd_edge_confidential_body_plain.json" >&2
  exit 1
}

seal_body() {
  local kid="$1"
  local secret="$2"
  "${CONF_TOOL_BIN}" --seal --kid "${kid}" --secret "${secret}"
}

encrypted_hello_body="$(
  seal_body "${CONF_KID}" "${CONF_SECRET}" <<JSON
{"node_id":"${NODE_ID}","model":"esp32sim_confidential","fw_git_sha":"encaccept","caps_sha256":"${CAPS_SHA}"}
JSON
)"

encrypted_hello_json="$(python3 - <<PY
import json, time, uuid
body_enc = json.loads(r'''${encrypted_hello_body}''')
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "NODE_HELLO",
  "from": "node:${NODE_ID}",
  "to": "platform",
  "body_enc": body_enc,
}))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${encrypted_hello_json}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

encrypted_caps_body="$(
  seal_body "${CONF_KID}" "${CONF_SECRET}" <<JSON
{
  "node_id":"${NODE_ID}",
  "manifest":{
    "spec_version":"um-acds/0.1",
    "manifest_version":"0.0.1",
    "caps_sha256":"${CAPS_SHA}",
    "node":{"node_id":"${NODE_ID}"},
    "runtime":{"agent_core":{"version":"0.0.0"}},
    "hardware":{"presence":{"sensor.temp":"present"}},
    "tools":[
      {
        "name":"sensor.temp.read",
        "kind":"sensor",
        "description":"Confidential body smoke tool",
        "parameters_schema":{"type":"object","additionalProperties":false,"properties":{}},
        "timeout_ms":500,
        "idempotent":true,
        "side_effect_level":"none",
        "hazards":[]
      }
    ],
    "safety":{},
    "tags":["edge:confidential"]
  }
}
JSON
)"

encrypted_caps_json="$(python3 - <<PY
import json, time, uuid
body_enc = json.loads(r'''${encrypted_caps_body}''')
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "NODE_CAPS_RSP",
  "from": "node:${NODE_ID}",
  "to": "platform",
  "body_enc": body_enc,
}))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${encrypted_caps_json}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

node_json="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/node?node_id=${NODE_ID}")"
bundle_json="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/node/manifest_bundle?node_id=${NODE_ID}")"

python3 - <<PY
import json, sys
node_obj = json.loads(r'''${node_json}''')
bundle_obj = json.loads(r'''${bundle_json}''')
if not node_obj.get("ok"):
  print("node lookup failed", node_obj, file=sys.stderr)
  raise SystemExit(1)
if (node_obj.get("node") or {}).get("node_id") != "${NODE_ID}":
  print("wrong node lookup payload", node_obj, file=sys.stderr)
  raise SystemExit(1)
bundle = bundle_obj.get("bundle") or {}
if not bundle_obj.get("ok") or bundle.get("node_id") != "${NODE_ID}":
  print("manifest bundle lookup failed", bundle_obj, file=sys.stderr)
  raise SystemExit(1)
tool_names = [t.get("name") for t in (bundle.get("tools") or []) if isinstance(t, dict)]
if "sensor.temp.read" not in tool_names:
  print("missing confidential tool in bundle", bundle, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

wrong_secret_body="$(
  seal_body "${CONF_KID}" "wrong-secret" <<JSON
{"node_id":"${NODE_ID}","model":"esp32sim_confidential","fw_git_sha":"wrongsecret","caps_sha256":"${CAPS_SHA}"}
JSON
)"

wrong_secret_json="$(python3 - <<PY
import json, time, uuid
body_enc = json.loads(r'''${wrong_secret_body}''')
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "NODE_HELLO",
  "from": "node:${NODE_ID}",
  "to": "platform",
  "body_enc": body_enc,
}))
PY
)"

wrong_status="$(curl -sS --noproxy "*" --max-time 10 -o "${LOG_DIR}/agentd_edge_confidential_body_wrong_secret.json" -w "%{http_code}" \
  -H "Content-Type: application/json" \
  -d "${wrong_secret_json}" \
  "${DAEMON_URL}/api/v1/edge/message")"
[[ "${wrong_status}" == "401" ]] || {
  echo "expected 401 for wrong-secret encrypted body, got ${wrong_status}" >&2
  cat "${LOG_DIR}/agentd_edge_confidential_body_wrong_secret.json" >&2
  exit 1
}

unknown_kid_body="$(
  seal_body "node-confidential:missing" "${CONF_SECRET}" <<JSON
{"node_id":"${NODE_ID}","model":"esp32sim_confidential","fw_git_sha":"unknownkid","caps_sha256":"${CAPS_SHA}"}
JSON
)"

unknown_kid_json="$(python3 - <<PY
import json, time, uuid
body_enc = json.loads(r'''${unknown_kid_body}''')
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "NODE_HELLO",
  "from": "node:${NODE_ID}",
  "to": "platform",
  "body_enc": body_enc,
}))
PY
)"

unknown_status="$(curl -sS --noproxy "*" --max-time 10 -o "${LOG_DIR}/agentd_edge_confidential_body_unknown_kid.json" -w "%{http_code}" \
  -H "Content-Type: application/json" \
  -d "${unknown_kid_json}" \
  "${DAEMON_URL}/api/v1/edge/message")"
[[ "${unknown_status}" == "401" ]] || {
  echo "expected 401 for unknown confidential kid, got ${unknown_status}" >&2
  cat "${LOG_DIR}/agentd_edge_confidential_body_unknown_kid.json" >&2
  exit 1
}
