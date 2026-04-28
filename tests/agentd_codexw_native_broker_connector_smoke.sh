#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/agentd-codexw-native.XXXXXX")"
cleanup() {
  rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

KEY_PATH="${TMP_DIR}/deployment.key.pem"
CERT_PATH="${TMP_DIR}/deployment.cert.pem"
OUT_PATH="${TMP_DIR}/dry-run.json"

openssl ecparam -name prime256v1 -genkey -noout -out "${KEY_PATH}" >/dev/null 2>&1
openssl req -new -x509 \
  -key "${KEY_PATH}" \
  -out "${CERT_PATH}" \
  -days 1 \
  -subj "/CN=agentd-native-smoke" \
  >/dev/null 2>&1

"${SCRIPT_DIR}/../tools/agentd_codexw_native_broker_connector.py" \
  --broker-url "http://127.0.0.1:8787" \
  --deployment-id "agentd-native-smoke" \
  --display-name "agentd native smoke" \
  --runtime-instance-id "agentd-native-smoke-instance" \
  --deployment-cert-path "${CERT_PATH}" \
  --deployment-key-path "${KEY_PATH}" \
  --agentd-base-url "http://127.0.0.1:18080" \
  --timestamp 1700000000 \
  --dry-run \
  >"${OUT_PATH}"

python3 - <<PY
import base64
import json
import re
import sys
from pathlib import Path

payload = json.loads(Path("${OUT_PATH}").read_text())
headers = payload["connect_headers"]
runtime = payload["runtime_snapshot"]["runtime"]
frame = payload["deployment_snapshot_frame"]

if payload.get("mode") != "dry_run":
    print("bad mode", payload, file=sys.stderr)
    raise SystemExit(1)
if payload.get("runtime_capabilities_hash") != payload.get("runtime_capabilities_canonical_json_sha256"):
    print("hash mismatch", payload, file=sys.stderr)
    raise SystemExit(1)
if not re.fullmatch(r"[0-9a-f]{64}", payload.get("runtime_capabilities_hash", "")):
    print("bad capability hash", payload, file=sys.stderr)
    raise SystemExit(1)
if headers.get("X-Codexw-Runtime-Kind") != "agentd":
    print("bad runtime kind header", headers, file=sys.stderr)
    raise SystemExit(1)
if headers.get("X-Codexw-Runtime-Instance-Id") != "agentd-native-smoke-instance":
    print("bad instance header", headers, file=sys.stderr)
    raise SystemExit(1)
if headers.get("X-Codexw-Runtime-Capabilities-SHA256") != payload["runtime_capabilities_hash"]:
    print("bad capability header", headers, file=sys.stderr)
    raise SystemExit(1)
if headers.get("X-Codexw-Deployment-Id") != "agentd-native-smoke":
    print("bad deployment id header", headers, file=sys.stderr)
    raise SystemExit(1)
for key in ("X-Codexw-Deployment-Certificate", "X-Codexw-Deployment-Certificate-Signature"):
    try:
        base64.b64decode(headers.get(key, ""), validate=True)
    except Exception as exc:
        print(f"bad base64 header {key}: {exc}", headers, file=sys.stderr)
        raise SystemExit(1)
if runtime.get("kind") != "agentd" or runtime.get("runtime_kind") != "agentd":
    print("bad runtime snapshot", runtime, file=sys.stderr)
    raise SystemExit(1)
if runtime.get("runtime_capabilities", {}).get("schema") != "broker.runtime_capabilities.v1":
    print("bad runtime capabilities", runtime, file=sys.stderr)
    raise SystemExit(1)
if frame.get("type") != "deployment.snapshot" or frame.get("deployment_id") != "agentd-native-smoke":
    print("bad snapshot frame", frame, file=sys.stderr)
    raise SystemExit(1)
if "workflow.submit" not in payload["runtime_capabilities"]["actions"]:
    print("missing workflow action", payload["runtime_capabilities"], file=sys.stderr)
    raise SystemExit(1)
PY

echo "agentd_codexw_native_broker_connector_smoke OK"
