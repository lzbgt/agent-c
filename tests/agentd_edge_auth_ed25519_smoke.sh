#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
ED_TOOL="${2:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "missing agentd binary path arg" >&2
  exit 2
fi
if [[ -z "${ED_TOOL}" ]]; then
  echo "missing agent_ed25519_tool binary path arg" >&2
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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_auth_ed25519_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

NODE_ID="node_ed25519_1"
KID_NODE="${NODE_ID}"
CAPS_SHA="sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

# Fixed seed for deterministic test signatures (32 bytes => 64 hex chars).
SK_HEX="9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"

PK_B64="$("${ED_TOOL}" --sk-hex "${SK_HEX}" --print-pk-b64 | tr -d '\r\n')"
if [[ -z "${PK_B64}" ]]; then
  echo "failed to compute PK_B64" >&2
  exit 1
fi

# Configure auth-required and provision a pubkey entry.
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
  "edge_auth_ed25519_pubkeys": {"${KID_NODE}": "${PK_B64}"},
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
hello_signed="$(ED25519_TOOL="${ED_TOOL}" python3 - <<PY
import json, os, subprocess, uuid, time

tool = os.environ["ED25519_TOOL"]
sk_hex = "${SK_HEX}"
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
env["auth"] = {"alg": "ed25519", "kid": "${KID_NODE}", "seq": 1}
canon = json.dumps(env, sort_keys=True, separators=(",", ":")).encode("utf-8")
sig_b64 = subprocess.check_output([tool, "--sk-hex", sk_hex], input=canon).decode("utf-8").strip()
env["auth"]["sig"] = sig_b64
print(json.dumps(env))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${hello_signed}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

# Retrying the exact same message (same msg_id, same seq) must remain idempotent.
curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${hello_signed}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

# Replaying with a new msg_id but the same seq must be rejected (401).
hello_replay_same_seq="$(ED25519_TOOL="${ED_TOOL}" python3 - <<PY
import json, os, subprocess, uuid, time
tool = os.environ["ED25519_TOOL"]
sk_hex = "${SK_HEX}"
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
env["auth"] = {"alg": "ed25519", "kid": "${KID_NODE}", "seq": 1}
canon = json.dumps(env, sort_keys=True, separators=(",", ":")).encode("utf-8")
sig_b64 = subprocess.check_output([tool, "--sk-hex", sk_hex], input=canon).decode("utf-8").strip()
env["auth"]["sig"] = sig_b64
print(json.dumps(env))
PY
)"

status_replay_same_seq="$(
  curl -sS --noproxy "*" --max-time 10 \
    -o /dev/null -w "%{http_code}" \
    -H "Content-Type: application/json" \
    -d "${hello_replay_same_seq}" \
    "${DAEMON_URL}/api/v1/edge/message"
)"
if [[ "${status_replay_same_seq}" != "401" ]]; then
  echo "expected 401 for replay with same seq and new msg_id, got ${status_replay_same_seq}" >&2
  exit 1
fi

# Also accept a CBOR-native signing profile: Ed25519 over deterministic CBOR encoding of the envelope with auth.sig removed.
hello_signed_cbor_alg="$(ED25519_TOOL="${ED_TOOL}" python3 - <<PY
import json, os, subprocess, uuid, time

tool = os.environ["ED25519_TOOL"]
sk_hex = "${SK_HEX}"

def _cbor_ai_u64(major_shift, n):
  if n < 24:
    return bytes([major_shift | n])
  if n <= 0xff:
    return bytes([major_shift | 24, n])
  if n <= 0xffff:
    return bytes([major_shift | 25, (n >> 8) & 0xff, n & 0xff])
  if n <= 0xffffffff:
    return bytes([major_shift | 26, (n >> 24) & 0xff, (n >> 16) & 0xff, (n >> 8) & 0xff, n & 0xff])
  return bytes([major_shift | 27]) + n.to_bytes(8, "big", signed=False)

def _cbor_text(s):
  b = s.encode("utf-8")
  return _cbor_ai_u64(3 << 5, len(b)) + b

def _cbor_array(a):
  out = _cbor_ai_u64(4 << 5, len(a))
  for v in a:
    out += _cbor_any(v)
  return out

def _cbor_map(m):
  # Must match daemon/src/cbor_encode.* deterministic key ordering:
  # - sort by UTF-8 byte length
  # - then lexicographically by UTF-8 bytes
  keys = sorted(m.keys(), key=lambda k: (len(k.encode("utf-8")), k.encode("utf-8")))
  out = _cbor_ai_u64(5 << 5, len(keys))
  for k in keys:
    out += _cbor_text(k)
    out += _cbor_any(m[k])
  return out

def _cbor_any(v):
  if v is None:
    return b"\xf6"
  if v is True:
    return b"\xf5"
  if v is False:
    return b"\xf4"
  if isinstance(v, int):
    if v >= 0:
      return _cbor_ai_u64(0 << 5, v)
    return _cbor_ai_u64(1 << 5, -1 - v)
  if isinstance(v, str):
    return _cbor_text(v)
  if isinstance(v, list):
    return _cbor_array(v)
  if isinstance(v, dict):
    return _cbor_map(v)
  raise TypeError("unsupported type for test cbor encoder: %r" % (type(v),))

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
env["auth"] = {"alg": "ed25519-cbor", "kid": "${KID_NODE}", "seq": 2}
cbor = _cbor_any(env)
sig_b64 = subprocess.check_output([tool, "--sk-hex", sk_hex], input=cbor).decode("utf-8").strip()
env["auth"]["sig"] = sig_b64
print(json.dumps(env))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${hello_signed_cbor_alg}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

echo "agentd_edge_auth_ed25519_smoke OK" >&2
