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

export AGENTD_RUN_ATTEST_HMAC_KID="edge-auth-bundle-send-k0"
export AGENTD_RUN_ATTEST_HMAC_KEY="edge-auth-bundle-send-secret"

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_auth_bundle_send_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

TARGET_NODE_ID="node_auth_bundle_target_1"
TARGET_CAPS_SHA="sha256:cececececececececececececececececececececececececececececececece"
CONF_KID="${TARGET_NODE_ID}:enc"
CONF_SECRET="edge-auth-bundle-confidential-secret"

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
    "model": "esp32sim_bundle_send",
    "fw_git_sha": "bundle-send-f00d",
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
  "hardware": {"presence": {"radio.ble": "present"}},
  "tools": [
    {
      "name": "sensor.bundle.recv",
      "kind": "sensor",
      "description": "Bundle receive smoke tool",
      "parameters_schema": {"type":"object","additionalProperties": False, "properties": {}},
      "timeout_ms": 500,
      "idempotent": True,
      "side_effect_level": "none",
      "hazards": []
    }
  ],
  "safety": {},
  "tags": ["edge:recipient"]
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

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "edge_auth_required": True,
  "edge_auth_require_ts": True,
  "edge_auth_max_skew_ms": 60000,
  "edge_auth_require_seq": True,
  "edge_auth_kid_policy": "node_prefix",
}))
PY
)" \
  "${DAEMON_URL}/api/v1/config/update" >/dev/null

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "mode": "replace",
  "rotation_epoch": 7,
  "edge_auth_hmac_keys": {
    "${TARGET_NODE_ID}:v1": "recipient-secret-v1"
  },
}))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/auth/trust_roots/rotate" >/dev/null

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "mode": "replace",
  "rotation_epoch": 5,
  "revoked_kids": ["${TARGET_NODE_ID}:revoked"],
  "revoked_node_ids": ["node_revoked_elsewhere"],
}))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/auth/revocations/update" >/dev/null

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

trust_send_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "target_node_id": "${TARGET_NODE_ID}",
}))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/auth/trust_roots/send")"

revoke_send_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "target_node_id": "${TARGET_NODE_ID}",
}))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/auth/revocations/send")"

python3 - <<PY
import json, sys
trust = json.loads(r'''${trust_send_resp}''')
revoke = json.loads(r'''${revoke_send_resp}''')
for name, obj, field in [
  ("trust", trust, "trust_roots"),
  ("revoke", revoke, "revocations"),
]:
  if not obj.get("ok"):
    print(f"{name} send not ok", obj, file=sys.stderr)
    raise SystemExit(1)
  if obj.get("target_node_id") != "${TARGET_NODE_ID}":
    print(f"{name} send wrong target", obj, file=sys.stderr)
    raise SystemExit(1)
  if not isinstance(obj.get("outbox_id"), int) or obj.get("outbox_id", 0) <= 0:
    print(f"{name} send missing outbox_id", obj, file=sys.stderr)
    raise SystemExit(1)
  if not isinstance(obj.get(field), dict):
    print(f"{name} send missing bundle", obj, file=sys.stderr)
    raise SystemExit(1)
print("ok")
PY

trust_send_conf_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "target_node_id": "${TARGET_NODE_ID}",
  "confidential_kid": "${CONF_KID}",
}))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/auth/trust_roots/send")"

revoke_send_conf_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "target_node_id": "${TARGET_NODE_ID}",
  "confidential_kid": "${CONF_KID}",
}))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/auth/revocations/send")"

python3 - <<PY
import json, sys
trust = json.loads(r'''${trust_send_conf_resp}''')
revoke = json.loads(r'''${revoke_send_conf_resp}''')
for name, obj in [("trust", trust), ("revoke", revoke)]:
  if not obj.get("ok"):
    print(f"{name} encrypted send not ok", obj, file=sys.stderr)
    raise SystemExit(1)
  if obj.get("confidential_kid") != "${CONF_KID}":
    print(f"{name} missing confidential_kid echo", obj, file=sys.stderr)
    raise SystemExit(1)
print("ok")
PY

conf_outbox_json="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/outbox?node_id=${TARGET_NODE_ID}&cursor=0&limit=100")"

conf_payloads_json="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${conf_outbox_json}''')
msgs = obj.get("messages") or []
out = {}
for row in msgs:
  candidate = row.get("msg") or {}
  body_enc = candidate.get("body_enc")
  if not (isinstance(body_enc, dict) and body_enc.get("kid") == "${CONF_KID}"):
    continue
  if candidate.get("type") == "PLATFORM_TRUST_ROOTS_BUNDLE":
    out["trust"] = body_enc
  elif candidate.get("type") == "PLATFORM_REVOCATIONS_BUNDLE":
    out["revoke"] = body_enc
if "trust" not in out or "revoke" not in out:
  print("missing encrypted auth bundles", msgs, file=sys.stderr)
  raise SystemExit(1)
print(json.dumps(out))
PY
)"

trust_opened_json="$(python3 - <<PY
import json, subprocess, sys
payload = json.loads(r'''${conf_payloads_json}''')
p = subprocess.run(
  ["${CONF_TOOL_BIN}", "--open", "--key", "${CONF_KID}=${CONF_SECRET}"],
  input=json.dumps(payload["trust"]).encode("utf-8"),
  stdout=subprocess.PIPE,
  stderr=subprocess.PIPE,
  check=True,
)
sys.stdout.write(p.stdout.decode("utf-8"))
PY
)"

revoke_opened_json="$(python3 - <<PY
import json, subprocess, sys
payload = json.loads(r'''${conf_payloads_json}''')
p = subprocess.run(
  ["${CONF_TOOL_BIN}", "--open", "--key", "${CONF_KID}=${CONF_SECRET}"],
  input=json.dumps(payload["revoke"]).encode("utf-8"),
  stdout=subprocess.PIPE,
  stderr=subprocess.PIPE,
  check=True,
)
sys.stdout.write(p.stdout.decode("utf-8"))
PY
)"

python3 - <<PY
import json, sys
trust_body = json.loads(r'''${trust_opened_json}''')
revoke_body = json.loads(r'''${revoke_opened_json}''')
trust_bundle = trust_body.get("trust_roots") or {}
revoke_bundle = revoke_body.get("revocations") or {}
if trust_bundle.get("schema") != "edge_auth_trust_roots_v1":
  print("wrong decrypted trust bundle schema", trust_body, file=sys.stderr)
  raise SystemExit(1)
if trust_bundle.get("rotation_epoch") != 7:
  print("wrong decrypted trust bundle epoch", trust_bundle, file=sys.stderr)
  raise SystemExit(1)
if revoke_bundle.get("schema") != "edge_auth_revocations_v1":
  print("wrong decrypted revocation bundle schema", revoke_body, file=sys.stderr)
  raise SystemExit(1)
if revoke_bundle.get("rotation_epoch") != 5:
  print("wrong decrypted revocation bundle epoch", revoke_bundle, file=sys.stderr)
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
trust_msg = None
revoke_msg = None
for row in msgs:
  candidate = row.get("msg") or {}
  if candidate.get("type") == "PLATFORM_TRUST_ROOTS_BUNDLE" and isinstance((candidate.get("body") or {}).get("trust_roots"), dict):
    trust_msg = candidate
  if candidate.get("type") == "PLATFORM_REVOCATIONS_BUNDLE" and isinstance((candidate.get("body") or {}).get("revocations"), dict):
    revoke_msg = candidate
if trust_msg is None:
  print("missing trust-roots outbox message", msgs, file=sys.stderr)
  raise SystemExit(1)
if revoke_msg is None:
  print("missing revocations outbox message", msgs, file=sys.stderr)
  raise SystemExit(1)
for name, msg in [("trust", trust_msg), ("revoke", revoke_msg)]:
  if msg.get("to") != "node:${TARGET_NODE_ID}":
    print(f"{name} wrong outbox destination", msg, file=sys.stderr)
    raise SystemExit(1)

trust_bundle = (trust_msg.get("body") or {}).get("trust_roots") or {}
if trust_bundle.get("schema") != "edge_auth_trust_roots_v1":
  print("wrong trust bundle schema", trust_bundle, file=sys.stderr)
  raise SystemExit(1)
if trust_bundle.get("rotation_epoch") != 7:
  print("wrong trust rotation epoch", trust_bundle, file=sys.stderr)
  raise SystemExit(1)
if trust_bundle.get("hmac_kids") != ["${TARGET_NODE_ID}:v1"]:
  print("wrong trust hmac_kids", trust_bundle.get("hmac_kids"), file=sys.stderr)
  raise SystemExit(1)
trust_att = trust_bundle.get("attest") or {}
if trust_att.get("alg") != "hmac-sha256" or trust_att.get("kid") != "edge-auth-bundle-send-k0":
  print("wrong trust attest", trust_att, file=sys.stderr)
  raise SystemExit(1)
trust_signable = dict(trust_bundle)
trust_signable.pop("attest", None)
trust_canon = json.dumps(trust_signable, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
trust_expected = base64.b64encode(hmac.new(b"edge-auth-bundle-send-secret", trust_canon, hashlib.sha256).digest()).decode("ascii")
if trust_att.get("sig") != trust_expected:
  print("trust signature mismatch", trust_att.get("sig"), trust_expected, file=sys.stderr)
  raise SystemExit(1)

revoke_bundle = (revoke_msg.get("body") or {}).get("revocations") or {}
if revoke_bundle.get("schema") != "edge_auth_revocations_v1":
  print("wrong revocation bundle schema", revoke_bundle, file=sys.stderr)
  raise SystemExit(1)
if revoke_bundle.get("rotation_epoch") != 5:
  print("wrong revocation rotation epoch", revoke_bundle, file=sys.stderr)
  raise SystemExit(1)
if revoke_bundle.get("revoked_kids") != ["${TARGET_NODE_ID}:revoked"]:
  print("wrong revoked_kids", revoke_bundle.get("revoked_kids"), file=sys.stderr)
  raise SystemExit(1)
if revoke_bundle.get("revoked_node_ids") != ["node_revoked_elsewhere"]:
  print("wrong revoked_node_ids", revoke_bundle.get("revoked_node_ids"), file=sys.stderr)
  raise SystemExit(1)
revoke_att = revoke_bundle.get("attest") or {}
if revoke_att.get("alg") != "hmac-sha256" or revoke_att.get("kid") != "edge-auth-bundle-send-k0":
  print("wrong revocation attest", revoke_att, file=sys.stderr)
  raise SystemExit(1)
revoke_signable = dict(revoke_bundle)
revoke_signable.pop("attest", None)
revoke_canon = json.dumps(revoke_signable, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
revoke_expected = base64.b64encode(hmac.new(b"edge-auth-bundle-send-secret", revoke_canon, hashlib.sha256).digest()).decode("ascii")
if revoke_att.get("sig") != revoke_expected:
  print("revocation signature mismatch", revoke_att.get("sig"), revoke_expected, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY
