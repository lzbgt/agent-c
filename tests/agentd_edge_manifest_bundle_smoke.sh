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

export AGENTD_RUN_ATTEST_HMAC_KID="edge-manifest-k0"
export AGENTD_RUN_ATTEST_HMAC_KEY="edge-manifest-secret"

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_manifest_bundle_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

NODE_ID="node_manifest_1"
CAPS_SHA="sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"

hello_json="$(python3 - <<PY
import json, uuid, time
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "NODE_HELLO",
  "from": f"node:{'${NODE_ID}'}",
  "to": "platform",
  "body": {
    "node_id": "${NODE_ID}",
    "model": "esp32sim_manifest",
    "fw_git_sha": "cafef00d",
    "caps_sha256": "${CAPS_SHA}",
  }
}))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${hello_json}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

caps_rsp_json="$(python3 - <<PY
import json, uuid, time
manifest = {
  "spec_version": "um-acds/0.1",
  "manifest_version": "0.0.1",
  "caps_sha256": "${CAPS_SHA}",
  "node": {"node_id": "${NODE_ID}"},
  "runtime": {"agent_core": {"version": "0.0.0"}},
  "hardware": {"presence": {"sensor.temp": "present", "ui.led.ws2812": "present"}},
  "tools": [
    {
      "name": "sensor.temp.read",
      "kind": "sensor",
      "description": "Read temperature",
      "parameters_schema": {"type":"object","additionalProperties": False, "properties": {}},
      "timeout_ms": 500,
      "idempotent": True,
      "side_effect_level": "none",
      "hazards": []
    }
  ],
  "safety": {},
  "tags": ["room:lobby", "class:sensor"]
}
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "NODE_CAPS_RSP",
  "from": f"node:{'${NODE_ID}'}",
  "to": "platform",
  "body": {"node_id": "${NODE_ID}", "manifest": manifest},
}))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${caps_rsp_json}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

bundle_get="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/node/manifest_bundle?node_id=${NODE_ID}")"

python3 - <<PY
import base64, hashlib, hmac, json, sys

obj = json.loads(r'''${bundle_get}''')
if not obj.get("ok"):
  print("manifest_bundle not ok:", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("node_id") != "${NODE_ID}":
  print("wrong node id:", obj, file=sys.stderr)
  raise SystemExit(1)

bundle = obj.get("bundle") or {}
if bundle.get("schema") != "edge_node_manifest_bundle_v1":
  print("wrong bundle schema:", bundle, file=sys.stderr)
  raise SystemExit(1)
if bundle.get("caps_sha256") != "${CAPS_SHA}":
  print("wrong caps sha:", bundle, file=sys.stderr)
  raise SystemExit(1)
if bundle.get("model") != "esp32sim_manifest":
  print("wrong model:", bundle, file=sys.stderr)
  raise SystemExit(1)

tools = bundle.get("tools") or []
tool_names = [t.get("name") for t in tools if isinstance(t, dict)]
if "sensor.temp.read" not in tool_names:
  print("expected derived tools surface:", tool_names, file=sys.stderr)
  raise SystemExit(1)

tags = bundle.get("tags") or []
if "room:lobby" not in tags:
  print("expected derived tags surface:", tags, file=sys.stderr)
  raise SystemExit(1)

presence = bundle.get("hardware_presence") or {}
if presence.get("sensor.temp") != "present":
  print("expected hardware presence surface:", presence, file=sys.stderr)
  raise SystemExit(1)

manifest = bundle.get("manifest") or {}
canon_manifest = json.dumps(manifest, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
manifest_sha = "sha256:" + hashlib.sha256(canon_manifest).hexdigest()
if bundle.get("manifest_sha256") != manifest_sha:
  print("manifest sha mismatch:", bundle.get("manifest_sha256"), manifest_sha, file=sys.stderr)
  raise SystemExit(1)

attest = bundle.get("attest") or {}
if attest.get("schema") != "edge_node_manifest_attest_v1":
  print("missing attest schema:", attest, file=sys.stderr)
  raise SystemExit(1)
if attest.get("alg") != "hmac-sha256":
  print("wrong attest alg:", attest, file=sys.stderr)
  raise SystemExit(1)
if attest.get("kid") != "edge-manifest-k0":
  print("wrong attest kid:", attest, file=sys.stderr)
  raise SystemExit(1)

signable = dict(bundle)
signable.pop("attest", None)
canon_bundle = json.dumps(signable, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
expected_sig = base64.b64encode(hmac.new(b"edge-manifest-secret", canon_bundle, hashlib.sha256).digest()).decode("ascii")
if attest.get("sig") != expected_sig:
  print("attest signature mismatch:", attest.get("sig"), expected_sig, file=sys.stderr)
  raise SystemExit(1)

print("ok")
PY
