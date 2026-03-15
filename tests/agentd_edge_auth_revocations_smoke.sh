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

export AGENTD_RUN_ATTEST_HMAC_KID="revocations-k0"
export AGENTD_RUN_ATTEST_HMAC_KEY="revocations-secret"

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_auth_revocations_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

NODE_ID="node_revoke_1"
KID_V1="${NODE_ID}:v1"
KID_V2="${NODE_ID}:v2"
SECRET_V1="node-revoke-secret-v1"
SECRET_V2="node-revoke-secret-v2"
CAPS_SHA="sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d '{"edge_auth_required":true,"edge_auth_require_ts":true,"edge_auth_max_skew_ms":60000,"edge_auth_kid_policy":"node_prefix"}' \
  "${DAEMON_URL}/api/v1/config/update" >/dev/null

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "node_id": "${NODE_ID}",
  "rotation_epoch": 1,
  "hmac_kid": "${KID_V1}",
  "hmac_secret": "${SECRET_V1}",
}))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/auth/provision_node" >/dev/null

hello_v1="$(python3 - <<PY
import base64, hashlib, hmac, json, uuid, time
env = {
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "NODE_HELLO",
  "from": f"node:{'${NODE_ID}'}",
  "to": "platform",
  "body": {
    "node_id": "${NODE_ID}",
    "model": "esp32sim_revocation",
    "fw_git_sha": "revocation-v1",
    "caps_sha256": "${CAPS_SHA}",
  }
}
env["auth"] = {"alg": "hmac-sha256", "kid": "${KID_V1}"}
canon = json.dumps(env, sort_keys=True, separators=(",", ":"))
mac = hmac.new(b"${SECRET_V1}", canon.encode("utf-8"), hashlib.sha256).digest()
env["auth"]["sig"] = base64.b64encode(mac).decode("ascii")
print(json.dumps(env))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${hello_v1}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

revoke_kid_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "mode": "merge",
  "rotation_epoch": 1,
  "revoked_kids": ["${KID_V1}"],
}))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/auth/revocations/update")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${revoke_kid_resp}''')
if not obj.get("ok"):
  print("revoke_kid_resp not ok", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("rotation_epoch") != 1:
  print("wrong revocation epoch", obj, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

revocations_bundle="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/auth/revocations")"

python3 - <<PY
import base64, hashlib, hmac, json, sys
obj = json.loads(r'''${revocations_bundle}''')
if not obj.get("ok"):
  print("revocations bundle not ok", obj, file=sys.stderr)
  raise SystemExit(1)
bundle = obj.get("revocations") or {}
if bundle.get("schema") != "edge_auth_revocations_v1":
  print("wrong revocation schema", bundle, file=sys.stderr)
  raise SystemExit(1)
if bundle.get("rotation_epoch") != 1:
  print("wrong bundle epoch", bundle, file=sys.stderr)
  raise SystemExit(1)
if bundle.get("revoked_kids") != ["${KID_V1}"]:
  print("unexpected revoked kids", bundle.get("revoked_kids"), file=sys.stderr)
  raise SystemExit(1)
if bundle.get("revoked_node_ids") != []:
  print("unexpected revoked node ids", bundle.get("revoked_node_ids"), file=sys.stderr)
  raise SystemExit(1)
att = bundle.get("attest") or {}
if att.get("alg") != "hmac-sha256" or att.get("kid") != "revocations-k0":
  print("wrong revocation attestation", att, file=sys.stderr)
  raise SystemExit(1)
signable = dict(bundle)
signable.pop("attest", None)
canon = json.dumps(signable, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
expected_sig = base64.b64encode(hmac.new(b"revocations-secret", canon, hashlib.sha256).digest()).decode("ascii")
if att.get("sig") != expected_sig:
  print("revocation attestation signature mismatch", file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

binding_after_kid_revoke="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/auth/node_binding?node_id=${NODE_ID}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${binding_after_kid_revoke}''')
b = obj.get("binding") or {}
if b.get("node_id_revoked") is not False:
  print("expected node not revoked yet", b, file=sys.stderr)
  raise SystemExit(1)
if b.get("matching_hmac_kids") != []:
  print("expected active matches cleared after kid revoke", b, file=sys.stderr)
  raise SystemExit(1)
if b.get("revoked_matching_hmac_kids") != ["${KID_V1}"]:
  print("expected revoked kid in binding", b, file=sys.stderr)
  raise SystemExit(1)
if b.get("kid_policy_satisfied") is not False:
  print("expected kid_policy_satisfied false after kid revoke", b, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

status_v1_revoked="$(
  curl -sS --noproxy "*" --max-time 10 \
    -o /dev/null -w "%{http_code}" \
    -H "Content-Type: application/json" \
    -d "${hello_v1}" \
    "${DAEMON_URL}/api/v1/edge/message"
)"
if [[ "${status_v1_revoked}" != "401" ]]; then
  echo "expected 401 for revoked kid, got ${status_v1_revoked}" >&2
  exit 1
fi

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "node_id": "${NODE_ID}",
  "rotation_epoch": 2,
  "hmac_kid": "${KID_V2}",
  "hmac_secret": "${SECRET_V2}",
}))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/auth/provision_node" >/dev/null

hello_v2="$(python3 - <<PY
import base64, hashlib, hmac, json, uuid, time
env = {
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "NODE_HELLO",
  "from": f"node:{'${NODE_ID}'}",
  "to": "platform",
  "body": {
    "node_id": "${NODE_ID}",
    "model": "esp32sim_revocation",
    "fw_git_sha": "revocation-v2",
    "caps_sha256": "${CAPS_SHA}",
  }
}
env["auth"] = {"alg": "hmac-sha256", "kid": "${KID_V2}"}
canon = json.dumps(env, sort_keys=True, separators=(",", ":"))
mac = hmac.new(b"${SECRET_V2}", canon.encode("utf-8"), hashlib.sha256).digest()
env["auth"]["sig"] = base64.b64encode(mac).decode("ascii")
print(json.dumps(env))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${hello_v2}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "mode": "merge",
  "rotation_epoch": 2,
  "revoked_node_ids": ["${NODE_ID}"],
}))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/auth/revocations/update" >/dev/null

binding_after_node_revoke="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/auth/node_binding?node_id=${NODE_ID}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${binding_after_node_revoke}''')
b = obj.get("binding") or {}
if b.get("node_id_revoked") is not True:
  print("expected node revoked", b, file=sys.stderr)
  raise SystemExit(1)
if b.get("matching_hmac_kids") != ["${KID_V2}"]:
  print("expected active v2 kid still visible", b, file=sys.stderr)
  raise SystemExit(1)
if b.get("kid_policy_satisfied") is not False:
  print("expected kid_policy_satisfied false under node revoke", b, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

status_v2_node_revoked="$(
  curl -sS --noproxy "*" --max-time 10 \
    -o /dev/null -w "%{http_code}" \
    -H "Content-Type: application/json" \
    -d "${hello_v2}" \
    "${DAEMON_URL}/api/v1/edge/message"
)"
if [[ "${status_v2_node_revoked}" != "401" ]]; then
  echo "expected 401 for revoked node id, got ${status_v2_node_revoked}" >&2
  exit 1
fi

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d '{"mode":"replace","rotation_epoch":3,"revoked_kids":[],"revoked_node_ids":[]}' \
  "${DAEMON_URL}/api/v1/edge/auth/revocations/update" >/dev/null

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${hello_v2}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null
