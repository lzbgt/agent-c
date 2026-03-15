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

CA_PEM_PATH="${ROOT}/tools/_compose_mtls/ca.pem"
SERVER_CERT_PATH="${ROOT}/tools/_compose_mtls/server.pem"
NON_CA_CERT_PATH="${ROOT}/tools/_compose_mtls/client.pem"
CA_PEM_JSON="$(python3 - <<PY
import json, pathlib
print(json.dumps(pathlib.Path("${CA_PEM_PATH}").read_text(encoding="utf-8")))
PY
)"
SERVER_CERT_JSON="$(python3 - <<PY
import json, pathlib
print(json.dumps(pathlib.Path("${SERVER_CERT_PATH}").read_text(encoding="utf-8")))
PY
)"
NON_CA_CERT_JSON="$(python3 - <<PY
import json, pathlib
print(json.dumps(pathlib.Path("${NON_CA_CERT_PATH}").read_text(encoding="utf-8")))
PY
)"

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_manifest_identity_cert_enforce_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

rotate_roots() {
  local epoch="$1"
  local pem_json="$2"
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/json" \
    -d "$(python3 - <<PY
import json
cert_pem = json.loads(r'''${pem_json}''')
print(json.dumps({
  "mode": "replace",
  "rotation_epoch": int(${epoch}),
  "edge_auth_cert_roots_pem": {"lab-ca": cert_pem},
}))
PY
)" \
    "${DAEMON_URL}/api/v1/edge/auth/cert_roots/rotate" >/dev/null
}

enable_manifest_enforcement() {
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/json" \
    -d '{"edge_auth_require_manifest_cert_chain":true}' \
    "${DAEMON_URL}/api/v1/config/update" >/dev/null
}

post_caps_rsp() {
  local node_id="$1"
  local caps_sha="$2"
  local cert_json="${3:-null}"
  local chain_json="${4:-null}"
  python3 - <<PY | curl -sS --noproxy "*" --max-time 10 -o "${LOG_DIR}/${node_id}.json" -w "%{http_code}" -H "Content-Type: application/json" -d @- "${DAEMON_URL}/api/v1/edge/message"
import json, time, uuid
node_id = "${node_id}"
caps_sha = "${caps_sha}"
cert_json = r'''${cert_json}'''
chain_json = r'''${chain_json}'''
manifest = {
  "spec_version": "um-acds/0.1",
  "manifest_version": "0.0.1",
  "caps_sha256": caps_sha,
  "node": {"node_id": node_id},
  "runtime": {"agent_core": {"version": "0.0.0"}},
  "hardware": {"presence": {"sensor.temp": "present"}},
  "tools": [{
    "name": "sensor.temp.read",
    "kind": "sensor",
    "description": "Read temperature",
    "parameters_schema": {"type": "object", "additionalProperties": False, "properties": {}},
    "timeout_ms": 500,
    "idempotent": True,
    "side_effect_level": "none",
    "hazards": []
  }],
  "safety": {},
  "tags": ["edge:manifest-cert"]
}
if cert_json != "null":
  identity = {"cert_pem": json.loads(cert_json)}
  if chain_json != "null":
    identity["cert_chain_pem"] = json.loads(chain_json)
  manifest["identity"] = identity
env = {
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "NODE_CAPS_RSP",
  "from": f"node:{node_id}",
  "to": "platform",
  "body": {"node_id": node_id, "manifest": manifest},
}
print(json.dumps(env))
PY
}

rotate_roots 11 "${CA_PEM_JSON}"
enable_manifest_enforcement

status_missing="$(post_caps_rsp "node_manifest_cert_missing" "sha256:1111111111111111111111111111111111111111111111111111111111111111")"
if [[ "${status_missing}" != "400" ]]; then
  echo "expected missing manifest cert to be rejected with 400, got ${status_missing}" >&2
  cat "${LOG_DIR}/node_manifest_cert_missing.json" >&2 || true
  exit 1
fi
python3 - <<PY
import json, pathlib, sys
obj = json.loads(pathlib.Path("${LOG_DIR}/node_manifest_cert_missing.json").read_text(encoding="utf-8"))
if "manifest.identity.cert_pem required" not in (obj.get("error") or ""):
  print("unexpected missing-cert error", obj, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

status_ok="$(post_caps_rsp "node_manifest_cert_ok" "sha256:3333333333333333333333333333333333333333333333333333333333333333" "${SERVER_CERT_JSON}")"
if [[ "${status_ok}" != "200" ]]; then
  echo "expected valid manifest cert chain to be accepted, got ${status_ok}" >&2
  cat "${LOG_DIR}/node_manifest_cert_ok.json" >&2 || true
  exit 1
fi

node_json="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/node?node_id=node_manifest_cert_ok")"
bundle_json="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/node/manifest_bundle?node_id=node_manifest_cert_ok")"

python3 - <<PY
import json, sys
node_obj = json.loads(r'''${node_json}''')
bundle_obj = json.loads(r'''${bundle_json}''')
verify = ((node_obj.get("node") or {}).get("identity_cert_verify")) or {}
if verify.get("verified") is not True:
  print("expected node identity_cert_verify.verified", verify, file=sys.stderr)
  raise SystemExit(1)
if "lab-ca" not in (verify.get("matched_root_kids") or []):
  print("expected matched root kid on node summary", verify, file=sys.stderr)
  raise SystemExit(1)
bundle_verify = ((bundle_obj.get("bundle") or {}).get("identity_cert_verify")) or {}
if bundle_verify.get("verified") is not True:
  print("expected bundle identity_cert_verify.verified", bundle_verify, file=sys.stderr)
  raise SystemExit(1)
if bundle_verify.get("leaf_sha256") != verify.get("leaf_sha256"):
  print("expected leaf sha to match across node/bundle", verify, bundle_verify, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

rotate_roots 12 "${NON_CA_CERT_JSON}"
status_after_replace="$(post_caps_rsp "node_manifest_cert_after_replace" "sha256:4444444444444444444444444444444444444444444444444444444444444444" "${SERVER_CERT_JSON}")"
if [[ "${status_after_replace}" != "400" ]]; then
  echo "expected manifest cert chain to fail after replacing roots with a non-CA cert, got ${status_after_replace}" >&2
  cat "${LOG_DIR}/node_manifest_cert_after_replace.json" >&2 || true
  exit 1
fi

echo "agentd_edge_manifest_identity_cert_enforce_smoke OK"
