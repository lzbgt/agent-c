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

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

export AGENTD_RUN_ATTEST_HMAC_KID="edge-manifest-send-k0"
export AGENTD_RUN_ATTEST_HMAC_KEY="edge-manifest-send-secret"

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_manifest_bundle_send_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

SUBJECT_NODE_ID="node_manifest_subject_1"
TARGET_NODE_ID="node_manifest_target_1"
SUBJECT_CAPS_SHA="sha256:abababababababababababababababababababababababababababababababab"
TARGET_CAPS_SHA="sha256:bcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbcbc"
CONF_KID="${TARGET_NODE_ID}:enc"
CONF_SECRET="manifest-send-confidential-secret"

register_node() {
  local node_id="$1"
  local caps_sha="$2"
  local tool_name="$3"
  local tag="$4"
  local presence_key="$5"
  local fw_sha="$6"

  local hello_json
  hello_json="$(python3 - <<PY
import json, uuid, time
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "NODE_HELLO",
  "from": f"node:{'${node_id}'}",
  "to": "platform",
  "body": {
    "node_id": "${node_id}",
    "model": "esp32sim_manifest_send",
    "fw_git_sha": "${fw_sha}",
    "caps_sha256": "${caps_sha}",
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
  "caps_sha256": "${caps_sha}",
  "node": {"node_id": "${node_id}"},
  "runtime": {"agent_core": {"version": "0.0.0"}},
  "hardware": {"presence": {"${presence_key}": "present"}},
  "tools": [
    {
      "name": "${tool_name}",
      "kind": "sensor",
      "description": "Manifest send smoke tool",
      "parameters_schema": {"type":"object","additionalProperties": False, "properties": {}},
      "timeout_ms": 500,
      "idempotent": True,
      "side_effect_level": "none",
      "hazards": []
    }
  ],
  "safety": {},
  "tags": ["${tag}"]
}
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "NODE_CAPS_RSP",
  "from": f"node:{'${node_id}'}",
  "to": "platform",
  "body": {"node_id": "${node_id}", "manifest": manifest},
}))
PY
)"

  curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/json" \
    -d "${caps_rsp_json}" \
    "${DAEMON_URL}/api/v1/edge/message" >/dev/null
}

register_node "${SUBJECT_NODE_ID}" "${SUBJECT_CAPS_SHA}" "sensor.temp.read" "room:lobby" "sensor.temp" "subjectf00d"
register_node "${TARGET_NODE_ID}" "${TARGET_CAPS_SHA}" "sensor.humidity.read" "room:hall" "sensor.humidity" "targetf00d"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "edge_confidentiality_keys": {
    "${CONF_KID}": "${CONF_SECRET}"
  }
}))
PY
)" \
  "${DAEMON_URL}/api/v1/config/update" >/dev/null

send_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "target_node_id": "${TARGET_NODE_ID}",
  "subject_node_id": "${SUBJECT_NODE_ID}",
}))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/node/manifest_bundle/send")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${send_resp}''')
if not obj.get("ok"):
  print("send response not ok", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("target_node_id") != "${TARGET_NODE_ID}" or obj.get("subject_node_id") != "${SUBJECT_NODE_ID}":
  print("wrong target/subject", obj, file=sys.stderr)
  raise SystemExit(1)
if not isinstance(obj.get("outbox_id"), int) or obj.get("outbox_id") <= 0:
  print("missing outbox_id", obj, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

enc_send_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "target_node_id": "${TARGET_NODE_ID}",
  "subject_node_id": "${SUBJECT_NODE_ID}",
  "confidential_kid": "${CONF_KID}",
}))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/node/manifest_bundle/send")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${enc_send_resp}''')
if not obj.get("ok"):
  print("encrypted send response not ok", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("confidential_kid") != "${CONF_KID}":
  print("missing confidential_kid echo", obj, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

enc_outbox_json="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/outbox?node_id=${TARGET_NODE_ID}&cursor=0&limit=100")"

enc_body_json="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${enc_outbox_json}''')
msgs = obj.get("messages") or []
for row in msgs:
  candidate = row.get("msg") or {}
  if candidate.get("type") != "PLATFORM_MANIFEST_BUNDLE":
    continue
  body_enc = candidate.get("body_enc")
  if isinstance(body_enc, dict) and body_enc.get("kid") == "${CONF_KID}":
    print(json.dumps(body_enc))
    raise SystemExit(0)
print("missing encrypted PLATFORM_MANIFEST_BUNDLE", file=sys.stderr)
raise SystemExit(1)
PY
)"

opened_body_json="$(printf '%s\n' "${enc_body_json}" | "${CONF_TOOL_BIN}" --open --key "${CONF_KID}=${CONF_SECRET}")"

python3 - <<PY
import json, sys
body = json.loads(r'''${opened_body_json}''')
if body.get("subject_node_id") != "${SUBJECT_NODE_ID}":
  print("wrong decrypted subject_node_id", body, file=sys.stderr)
  raise SystemExit(1)
bundle = body.get("bundle") or {}
if bundle.get("node_id") != "${SUBJECT_NODE_ID}":
  print("wrong decrypted bundle node_id", bundle, file=sys.stderr)
  raise SystemExit(1)
tool_names = [t.get("name") for t in (bundle.get("tools") or []) if isinstance(t, dict)]
if "sensor.temp.read" not in tool_names:
  print("missing tool in decrypted manifest bundle", bundle, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

outbox_json="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/outbox?node_id=${TARGET_NODE_ID}&cursor=0&limit=50")"

python3 - <<PY
import base64, hashlib, hmac, json, sys
obj = json.loads(r'''${outbox_json}''')
if not obj.get("ok"):
  print("outbox not ok", obj, file=sys.stderr)
  raise SystemExit(1)
msgs = obj.get("messages") or []
msg = None
for row in msgs:
  candidate = row.get("msg") or {}
  if candidate.get("type") == "PLATFORM_MANIFEST_BUNDLE":
    msg = candidate
    break
if msg is None:
  print("missing PLATFORM_MANIFEST_BUNDLE in outbox", msgs, file=sys.stderr)
  raise SystemExit(1)
if msg.get("to") != "node:${TARGET_NODE_ID}":
  print("wrong outbox destination", msg, file=sys.stderr)
  raise SystemExit(1)
body = msg.get("body") or {}
if body.get("subject_node_id") != "${SUBJECT_NODE_ID}":
  print("wrong subject_node_id in body", body, file=sys.stderr)
  raise SystemExit(1)
bundle = body.get("bundle") or {}
if bundle.get("schema") != "edge_node_manifest_bundle_v1":
  print("wrong bundle schema", bundle, file=sys.stderr)
  raise SystemExit(1)
if bundle.get("node_id") != "${SUBJECT_NODE_ID}":
  print("wrong subject bundle node_id", bundle, file=sys.stderr)
  raise SystemExit(1)
if bundle.get("caps_sha256") != "${SUBJECT_CAPS_SHA}":
  print("wrong subject caps sha", bundle, file=sys.stderr)
  raise SystemExit(1)
tags = bundle.get("tags") or []
if "room:lobby" not in tags:
  print("missing subject tags", tags, file=sys.stderr)
  raise SystemExit(1)
tools = bundle.get("tools") or []
tool_names = [t.get("name") for t in tools if isinstance(t, dict)]
if "sensor.temp.read" not in tool_names:
  print("missing subject tool", tool_names, file=sys.stderr)
  raise SystemExit(1)
attest = bundle.get("attest") or {}
if attest.get("alg") != "hmac-sha256" or attest.get("kid") != "edge-manifest-send-k0":
  print("wrong bundle attest", attest, file=sys.stderr)
  raise SystemExit(1)
signable = dict(bundle)
signable.pop("attest", None)
canon = json.dumps(signable, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
expected_sig = base64.b64encode(hmac.new(b"edge-manifest-send-secret", canon, hashlib.sha256).digest()).decode("ascii")
if attest.get("sig") != expected_sig:
  print("attest signature mismatch", attest.get("sig"), expected_sig, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY
