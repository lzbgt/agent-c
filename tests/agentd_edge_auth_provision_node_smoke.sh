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

export AGENTD_RUN_ATTEST_HMAC_KID="provision-k0"
export AGENTD_RUN_ATTEST_HMAC_KEY="provision-secret"

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_auth_provision_node_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

NODE_ID="node_bind_1"
PREFIX_KID="${NODE_ID}:v1"
SECRET="node-bind-secret"
CAPS_SHA="sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d '{"edge_auth_required":true,"edge_auth_require_ts":true,"edge_auth_max_skew_ms":60000,"edge_auth_require_seq":true,"edge_auth_kid_policy":"node_prefix"}' \
  "${DAEMON_URL}/api/v1/config/update" >/dev/null

binding_empty="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/auth/node_binding?node_id=${NODE_ID}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${binding_empty}''')
b = obj.get("binding") or {}
if not obj.get("ok"):
  print("binding_empty not ok", obj, file=sys.stderr)
  raise SystemExit(1)
if b.get("node_id") != "${NODE_ID}" or b.get("kid_policy") != "node_prefix":
  print("wrong initial binding", b, file=sys.stderr)
  raise SystemExit(1)
if b.get("matching_hmac_kids") != []:
  print("expected no keys initially", b, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

provision_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "node_id": "${NODE_ID}",
  "mode": "merge",
  "rotation_epoch": 1,
  "hmac_kid": "${PREFIX_KID}",
  "hmac_secret": "${SECRET}",
}))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/auth/provision_node")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${provision_resp}''')
if not obj.get("ok"):
  print("provision not ok", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("rotation_epoch") != 1:
  print("wrong rotation epoch", obj, file=sys.stderr)
  raise SystemExit(1)
binding = obj.get("binding") or {}
if binding.get("matching_hmac_kids") != ["${PREFIX_KID}"]:
  print("unexpected matching kids", binding, file=sys.stderr)
  raise SystemExit(1)
trust = obj.get("trust_roots") or {}
if trust.get("rotation_epoch") != 1:
  print("trust root epoch mismatch", trust, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

hello_signed="$(python3 - <<PY
import base64, hashlib, hmac, json, uuid, time
env = {
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "NODE_HELLO",
  "from": f"node:{'${NODE_ID}'}",
  "to": "platform",
  "body": {
    "node_id": "${NODE_ID}",
    "model": "esp32sim_provision",
    "fw_git_sha": "bindv1",
    "caps_sha256": "${CAPS_SHA}",
  }
}
env["auth"] = {"alg": "hmac-sha256", "kid": "${PREFIX_KID}", "seq": 1}
canon = json.dumps(env, sort_keys=True, separators=(",", ":"))
mac = hmac.new(b"${SECRET}", canon.encode("utf-8"), hashlib.sha256).digest()
env["auth"]["sig"] = base64.b64encode(mac).decode("ascii")
print(json.dumps(env))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${hello_signed}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

bad_provision_status="$(
  curl -sS --noproxy "*" --max-time 10 \
    -o /dev/null -w "%{http_code}" \
    -H "Content-Type: application/json" \
    -d "$(python3 - <<PY
import json
print(json.dumps({
  "node_id": "${NODE_ID}",
  "rotation_epoch": 2,
  "hmac_kid": "other-node",
  "hmac_secret": "wrong",
}))
PY
)" \
    "${DAEMON_URL}/api/v1/edge/auth/provision_node"
)"
if [[ "${bad_provision_status}" != "409" ]]; then
  echo "expected 409 for kid-policy violation, got ${bad_provision_status}" >&2
  exit 1
fi

binding_after="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/auth/node_binding?node_id=${NODE_ID}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${binding_after}''')
b = obj.get("binding") or {}
if b.get("matching_hmac_kids") != ["${PREFIX_KID}"]:
  print("binding changed unexpectedly", b, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY
