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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_auth_hmac_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

KID="k0"
SECRET="test_secret_123"
NODE_ID="node_auth_1"
CAPS_SHA="sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

# Configure auth-required and provision a keyring entry.
curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "edge_auth_required": True,
  "edge_auth_require_ts": True,
  "edge_auth_max_skew_ms": 60000,
  "edge_auth_hmac_keys": {"${KID}": "${SECRET}"},
}))
PY
)" \
  "${DAEMON_URL}/api/v1/config/update" >/dev/null

# Missing auth must be rejected (401).
hello_unsigned="$(python3 - <<PY
import json, uuid, time
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "NODE_HELLO",
  "from": f"node:{'${NODE_ID}'}",
  "to": "platform",
  "body": {
    "node_id": "${NODE_ID}",
    "model": "esp32sim_stub",
    "fw_git_sha": "deadbeef",
    "caps_sha256": "${CAPS_SHA}",
  }
}))
PY
)"

status_unsigned="$(
  curl -sS --noproxy "*" --max-time 10 \
    -o /dev/null -w "%{http_code}" \
    -H "Content-Type: application/json" \
    -d "${hello_unsigned}" \
    "${DAEMON_URL}/api/v1/edge/message"
)"
if [[ "${status_unsigned}" != "401" ]]; then
  echo "expected 401 for unsigned envelope, got ${status_unsigned}" >&2
  exit 1
fi

# Signed hello must be accepted.
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
    "model": "esp32sim_stub",
    "fw_git_sha": "deadbeef",
    "caps_sha256": "${CAPS_SHA}",
  }
}
canon = json.dumps(env, sort_keys=True, separators=(",", ":"))
mac = hmac.new(b"${SECRET}", canon.encode("utf-8"), hashlib.sha256).digest()
sig = base64.b64encode(mac).decode("ascii")
env["auth"] = {"alg": "hmac-sha256", "kid": "${KID}", "sig": sig}
print(json.dumps(env))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${hello_signed}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

nodes_json="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/nodes")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${nodes_json}''')
if not obj.get("ok"):
  print("nodes not ok:", obj, file=sys.stderr)
  raise SystemExit(1)
nodes = obj.get("nodes") or []
ids = [n.get("node_id") for n in nodes if isinstance(n, dict)]
if "${NODE_ID}" not in ids:
  print("expected node_id in nodes list, got:", ids, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

# Wrong signature must be rejected (401).
hello_bad_sig="$(python3 - <<PY
import base64, hashlib, hmac, json, uuid, time
env = {
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "NODE_HELLO",
  "from": f"node:{'${NODE_ID}'}",
  "to": "platform",
  "body": {
    "node_id": "${NODE_ID}",
    "model": "esp32sim_stub",
    "fw_git_sha": "deadbeef",
    "caps_sha256": "${CAPS_SHA}",
  }
}
canon = json.dumps(env, sort_keys=True, separators=(",", ":"))
mac = hmac.new(b"wrong_key", canon.encode("utf-8"), hashlib.sha256).digest()
sig = base64.b64encode(mac).decode("ascii")
env["auth"] = {"alg": "hmac-sha256", "kid": "${KID}", "sig": sig}
print(json.dumps(env))
PY
)"

status_bad_sig="$(
  curl -sS --noproxy "*" --max-time 10 \
    -o /dev/null -w "%{http_code}" \
    -H "Content-Type: application/json" \
    -d "${hello_bad_sig}" \
    "${DAEMON_URL}/api/v1/edge/message"
)"
if [[ "${status_bad_sig}" != "401" ]]; then
  echo "expected 401 for bad signature, got ${status_bad_sig}" >&2
  exit 1
fi

# Old timestamp outside skew must be rejected (401) even if HMAC verifies.
hello_old_ts="$(python3 - <<PY
import base64, hashlib, hmac, json, uuid, time
now_ms = int(time.time()*1000)
old_ms = now_ms - 10*60*1000
env = {
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": old_ms,
  "type": "NODE_HELLO",
  "from": f"node:{'${NODE_ID}'}",
  "to": "platform",
  "body": {
    "node_id": "${NODE_ID}",
    "model": "esp32sim_stub",
    "fw_git_sha": "deadbeef",
    "caps_sha256": "${CAPS_SHA}",
  }
}
canon = json.dumps(env, sort_keys=True, separators=(",", ":"))
mac = hmac.new(b"${SECRET}", canon.encode("utf-8"), hashlib.sha256).digest()
sig = base64.b64encode(mac).decode("ascii")
env["auth"] = {"alg": "hmac-sha256", "kid": "${KID}", "sig": sig}
print(json.dumps(env))
PY
)"

status_old_ts="$(
  curl -sS --noproxy "*" --max-time 10 \
    -o /dev/null -w "%{http_code}" \
    -H "Content-Type: application/json" \
    -d "${hello_old_ts}" \
    "${DAEMON_URL}/api/v1/edge/message"
)"
if [[ "${status_old_ts}" != "401" ]]; then
  echo "expected 401 for old ts outside skew, got ${status_old_ts}" >&2
  exit 1
fi

echo "ok"
