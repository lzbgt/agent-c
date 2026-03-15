#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
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

export AGENTD_RUN_ATTEST_HMAC_KID="edge-cert-roots-k0"
export AGENTD_RUN_ATTEST_HMAC_KEY="edge-cert-roots-secret"

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_auth_cert_roots_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

TARGET_NODE_ID="node_cert_roots_target_1"
TARGET_CAPS_SHA="sha256:f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1"
CERT_KID="lab-ca-1"
CERT_PEM_PATH="${ROOT}/tools/_compose_mtls/ca.pem"
CERT_PEM_JSON="$(python3 - <<PY
import json, pathlib
print(json.dumps(pathlib.Path("${CERT_PEM_PATH}").read_text(encoding="utf-8")))
PY
)"

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
    "model": "esp32sim_cert_roots",
    "fw_git_sha": "cert-roots-f00d",
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
  "hardware": {"presence": {"radio.wifi": "present"}},
  "tools": [
    {
      "name": "sensor.cert.recv",
      "kind": "sensor",
      "description": "Cert receive smoke tool",
      "parameters_schema": {"type":"object","additionalProperties": False, "properties": {}},
      "timeout_ms": 500,
      "idempotent": True,
      "side_effect_level": "none",
      "hazards": []
    }
  ],
  "safety": {},
  "tags": ["edge:cert-recipient"]
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
import json
cert_pem = json.loads(r'''${CERT_PEM_JSON}''')
print(json.dumps({
  "mode": "replace",
  "rotation_epoch": 3,
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
if obj.get("rotation_epoch") != 3:
  print("wrong rotation epoch", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("edge_auth_cert_roots_set") != 1:
  print("wrong cert roots set count", obj, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

get_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/auth/cert_roots")"

python3 - <<PY
import base64, hashlib, hmac, json, sys
obj = json.loads(r'''${get_resp}''')
if not obj.get("ok"):
  print("get not ok", obj, file=sys.stderr)
  raise SystemExit(1)
bundle = obj.get("cert_roots") or {}
if bundle.get("schema") != "edge_auth_cert_roots_v1":
  print("wrong bundle schema", bundle, file=sys.stderr)
  raise SystemExit(1)
if bundle.get("rotation_epoch") != 3:
  print("wrong rotation epoch", bundle, file=sys.stderr)
  raise SystemExit(1)
roots = bundle.get("cert_roots_pem") or {}
if sorted(roots.keys()) != ["${CERT_KID}"]:
  print("wrong cert root keys", roots, file=sys.stderr)
  raise SystemExit(1)
att = bundle.get("attest") or {}
if att.get("alg") != "hmac-sha256" or att.get("kid") != "edge-cert-roots-k0":
  print("wrong attest", att, file=sys.stderr)
  raise SystemExit(1)
signable = dict(bundle)
signable.pop("attest", None)
canon = json.dumps(signable, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
expected_sig = base64.b64encode(hmac.new(b"edge-cert-roots-secret", canon, hashlib.sha256).digest()).decode("ascii")
if att.get("sig") != expected_sig:
  print("signature mismatch", att.get("sig"), expected_sig, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

send_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "target_node_id": "${TARGET_NODE_ID}",
}))
PY
)" \
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
if not isinstance(obj.get("outbox_id"), int) or obj.get("outbox_id", 0) <= 0:
  print("missing outbox_id", obj, file=sys.stderr)
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
  if candidate.get("type") == "PLATFORM_CERT_ROOTS_BUNDLE":
    msg = candidate
    break
if msg is None:
  print("missing PLATFORM_CERT_ROOTS_BUNDLE", msgs, file=sys.stderr)
  raise SystemExit(1)
if msg.get("to") != "node:${TARGET_NODE_ID}":
  print("wrong destination", msg, file=sys.stderr)
  raise SystemExit(1)
bundle = (msg.get("body") or {}).get("cert_roots") or {}
if bundle.get("schema") != "edge_auth_cert_roots_v1":
  print("wrong outbox bundle schema", bundle, file=sys.stderr)
  raise SystemExit(1)
roots = bundle.get("cert_roots_pem") or {}
if sorted(roots.keys()) != ["${CERT_KID}"]:
  print("wrong outbox cert roots", roots, file=sys.stderr)
  raise SystemExit(1)
att = bundle.get("attest") or {}
if att.get("alg") != "hmac-sha256" or att.get("kid") != "edge-cert-roots-k0":
  print("wrong outbox attest", att, file=sys.stderr)
  raise SystemExit(1)
signable = dict(bundle)
signable.pop("attest", None)
canon = json.dumps(signable, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
expected_sig = base64.b64encode(hmac.new(b"edge-cert-roots-secret", canon, hashlib.sha256).digest()).decode("ascii")
if att.get("sig") != expected_sig:
  print("outbox signature mismatch", att.get("sig"), expected_sig, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY
