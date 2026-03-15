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

export AGENTD_RUN_ATTEST_HMAC_KID="trust-roots-k0"
export AGENTD_RUN_ATTEST_HMAC_KEY="trust-roots-secret"

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_auth_trust_roots_rotate_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

NODE_ID="node_rotate_1"
KID_NODE="${NODE_ID}"
SECRET_V1="secret_v1_rotate"
SECRET_V2="secret_v2_rotate"
CAPS_SHA="sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "edge_auth_required": True,
  "edge_auth_require_ts": True,
  "edge_auth_max_skew_ms": 60000,
  "edge_auth_require_seq": True,
  "edge_auth_kid_policy": "match_node",
}))
PY
)" \
  "${DAEMON_URL}/api/v1/config/update" >/dev/null

rotate_v1="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "mode": "replace",
  "rotation_epoch": 1,
  "edge_auth_hmac_keys": {"${KID_NODE}": "${SECRET_V1}"},
}))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/auth/trust_roots/rotate")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${rotate_v1}''')
if not obj.get("ok"):
  print("rotate_v1 not ok:", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("rotation_epoch") != 1:
  print("wrong rotation epoch:", obj, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

trust_roots_v1="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/auth/trust_roots")"

python3 - <<PY
import base64, hashlib, hmac, json, sys
obj = json.loads(r'''${trust_roots_v1}''')
if not obj.get("ok"):
  print("trust_roots_v1 not ok:", obj, file=sys.stderr)
  raise SystemExit(1)
bundle = obj.get("trust_roots") or {}
if bundle.get("schema") != "edge_auth_trust_roots_v1":
  print("wrong trust roots schema:", bundle, file=sys.stderr)
  raise SystemExit(1)
if bundle.get("rotation_epoch") != 1:
  print("wrong epoch in bundle:", bundle, file=sys.stderr)
  raise SystemExit(1)
if bundle.get("hmac_kids") != ["${KID_NODE}"]:
  print("wrong hmac kid set:", bundle.get("hmac_kids"), file=sys.stderr)
  raise SystemExit(1)
att = bundle.get("attest") or {}
if att.get("alg") != "hmac-sha256" or att.get("kid") != "trust-roots-k0":
  print("wrong bundle attestation:", att, file=sys.stderr)
  raise SystemExit(1)
signable = dict(bundle)
signable.pop("attest", None)
canon = json.dumps(signable, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
expected_sig = base64.b64encode(hmac.new(b"trust-roots-secret", canon, hashlib.sha256).digest()).decode("ascii")
if att.get("sig") != expected_sig:
  print("bundle attestation signature mismatch", att.get("sig"), expected_sig, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

hello_signed_v1="$(python3 - <<PY
import base64, hashlib, hmac, json, uuid, time
env = {
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "NODE_HELLO",
  "from": f"node:{'${NODE_ID}'}",
  "to": "platform",
  "body": {
    "node_id": "${NODE_ID}",
    "model": "esp32sim_rotate",
    "fw_git_sha": "rotatev1",
    "caps_sha256": "${CAPS_SHA}",
  }
}
env["auth"] = {"alg": "hmac-sha256", "kid": "${KID_NODE}", "seq": 1}
canon = json.dumps(env, sort_keys=True, separators=(",", ":"))
mac = hmac.new(b"${SECRET_V1}", canon.encode("utf-8"), hashlib.sha256).digest()
env["auth"]["sig"] = base64.b64encode(mac).decode("ascii")
print(json.dumps(env))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${hello_signed_v1}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "mode": "replace",
  "rotation_epoch": 2,
  "edge_auth_hmac_keys": {"${KID_NODE}": "${SECRET_V2}"},
}))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/auth/trust_roots/rotate" >/dev/null

hello_signed_old_key="$(python3 - <<PY
import base64, hashlib, hmac, json, uuid, time
env = {
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "NODE_HELLO",
  "from": f"node:{'${NODE_ID}'}",
  "to": "platform",
  "body": {
    "node_id": "${NODE_ID}",
    "model": "esp32sim_rotate",
    "fw_git_sha": "rotatev2-old",
    "caps_sha256": "${CAPS_SHA}",
  }
}
env["auth"] = {"alg": "hmac-sha256", "kid": "${KID_NODE}", "seq": 2}
canon = json.dumps(env, sort_keys=True, separators=(",", ":"))
mac = hmac.new(b"${SECRET_V1}", canon.encode("utf-8"), hashlib.sha256).digest()
env["auth"]["sig"] = base64.b64encode(mac).decode("ascii")
print(json.dumps(env))
PY
)"

status_old_key="$(
  curl -sS --noproxy "*" --max-time 10 \
    -o /dev/null -w "%{http_code}" \
    -H "Content-Type: application/json" \
    -d "${hello_signed_old_key}" \
    "${DAEMON_URL}/api/v1/edge/message"
)"
if [[ "${status_old_key}" != "401" ]]; then
  echo "expected 401 after trust-root rotation for old key, got ${status_old_key}" >&2
  exit 1
fi

hello_signed_v2="$(python3 - <<PY
import base64, hashlib, hmac, json, uuid, time
env = {
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "NODE_HELLO",
  "from": f"node:{'${NODE_ID}'}",
  "to": "platform",
  "body": {
    "node_id": "${NODE_ID}",
    "model": "esp32sim_rotate",
    "fw_git_sha": "rotatev2",
    "caps_sha256": "${CAPS_SHA}",
  }
}
env["auth"] = {"alg": "hmac-sha256", "kid": "${KID_NODE}", "seq": 2}
canon = json.dumps(env, sort_keys=True, separators=(",", ":"))
mac = hmac.new(b"${SECRET_V2}", canon.encode("utf-8"), hashlib.sha256).digest()
env["auth"]["sig"] = base64.b64encode(mac).decode("ascii")
print(json.dumps(env))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${hello_signed_v2}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

trust_roots_v2="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/auth/trust_roots")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${trust_roots_v2}''')
bundle = obj.get("trust_roots") or {}
if bundle.get("rotation_epoch") != 2:
  print("expected epoch 2:", bundle, file=sys.stderr)
  raise SystemExit(1)
if bundle.get("hmac_kids") != ["${KID_NODE}"]:
  print("unexpected kids after rotation:", bundle.get("hmac_kids"), file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY
