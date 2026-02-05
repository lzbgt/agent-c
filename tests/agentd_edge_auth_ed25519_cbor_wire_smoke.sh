#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
ENCODER_BIN="${2:-}"
ED_TOOL_BIN="${3:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "missing agentd binary path arg" >&2
  exit 2
fi
if [[ -z "${ENCODER_BIN}" || ! -x "${ENCODER_BIN}" ]]; then
  echo "missing/invalid encoder binary path arg" >&2
  exit 2
fi
if [[ -z "${ED_TOOL_BIN}" || ! -x "${ED_TOOL_BIN}" ]]; then
  echo "missing/invalid agent_ed25519_tool binary path arg" >&2
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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_auth_ed25519_cbor_wire_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

NODE_ID="node_auth_ed25519_cbor_1"
KID_NODE="${NODE_ID}"
SK_HEX="0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"
CAPS_SHA="sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

PK_B64="$("${ED_TOOL_BIN}" --sk-hex "${SK_HEX}" --print-pk-b64)"
if [[ -z "${PK_B64}" ]]; then
  echo "failed to compute pk base64" >&2
  exit 1
fi

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

# Unsigned CBOR must be rejected (401).
status_unsigned="$(
  "${ENCODER_BIN}" \
    --type NODE_HELLO \
    --node-id "${NODE_ID}" \
    --msg-id "cbor_ed_msg_0" \
    --caps-sha256 "${CAPS_SHA}" | \
  curl -sS --noproxy "*" --max-time 10 \
    -o /dev/null -w "%{http_code}" \
    -H "Content-Type: application/cbor" \
    --data-binary @- \
    "${DAEMON_URL}/api/v1/edge/message"
)"
if [[ "${status_unsigned}" != "401" ]]; then
  echo "expected 401 for unsigned CBOR envelope, got ${status_unsigned}" >&2
  exit 1
fi

# Signed CBOR (ed25519-cbor) must be accepted.
resp_signed="$(
  "${ENCODER_BIN}" \
    --type NODE_HELLO \
    --node-id "${NODE_ID}" \
    --msg-id "cbor_ed_msg_1" \
    --caps-sha256 "${CAPS_SHA}" \
    --auth-alg ed25519-cbor \
    --auth-kid "${KID_NODE}" \
    --auth-seq 1 \
    --ed25519-sk-hex "${SK_HEX}" | \
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/cbor" \
    --data-binary @- \
    "${DAEMON_URL}/api/v1/edge/message"
)"
python3 - <<PY
import json, sys
obj = json.loads(r'''${resp_signed}''')
if not obj.get("ok"):
  print("expected ok true", obj, file=sys.stderr)
  raise SystemExit(1)
PY

# Replaying with a new msg_id but the same seq must be rejected (401).
status_replay_same_seq="$(
  "${ENCODER_BIN}" \
    --type NODE_HELLO \
    --node-id "${NODE_ID}" \
    --msg-id "cbor_ed_msg_2" \
    --caps-sha256 "${CAPS_SHA}" \
    --auth-alg ed25519-cbor \
    --auth-kid "${KID_NODE}" \
    --auth-seq 1 \
    --ed25519-sk-hex "${SK_HEX}" | \
  curl -sS --noproxy "*" --max-time 10 \
    -o /dev/null -w "%{http_code}" \
    -H "Content-Type: application/cbor" \
    --data-binary @- \
    "${DAEMON_URL}/api/v1/edge/message"
)"
if [[ "${status_replay_same_seq}" != "401" ]]; then
  echo "expected 401 for CBOR replay with same seq and new msg_id, got ${status_replay_same_seq}" >&2
  exit 1
fi

echo "ok"

