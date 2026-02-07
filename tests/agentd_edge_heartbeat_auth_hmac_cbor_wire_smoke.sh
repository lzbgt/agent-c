#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
ENCODER_BIN="${2:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "missing agentd binary path arg" >&2
  exit 2
fi
if [[ -z "${ENCODER_BIN}" || ! -x "${ENCODER_BIN}" ]]; then
  echo "missing/non-executable encoder binary path arg: ${ENCODER_BIN}" >&2
  exit 2
fi

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

PORT_DAEMON="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"

trap agentd_smoke_stop EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_heartbeat_auth_hmac_cbor_wire_smoke" \
  --tools none
agentd_smoke_wait_health "${DAEMON_URL}"

NODE_ID="node_hb_auth_cbor_1"
KID_NODE="${NODE_ID}"
SECRET_NODE="test_secret_node_123"
CAPS_SHA="sha256:$(python3 - <<'PY'
print("a"*64)
PY
)"

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
  "edge_auth_hmac_keys": {"${KID_NODE}": "${SECRET_NODE}"},
}))
PY
)" \
  "${DAEMON_URL}/api/v1/config/update" >/dev/null

post_cbor_expect_ok() {
  local resp
  resp="$(curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/cbor" \
    --data-binary @- \
    "${DAEMON_URL}/api/v1/edge/message")"
  python3 - <<PY
import json, sys
obj = json.loads(r'''${resp}''')
if not obj.get("ok"):
  print("expected ok true", obj, file=sys.stderr)
  raise SystemExit(1)
PY
}

"${ENCODER_BIN}" \
  --type NODE_HELLO \
  --node-id "${NODE_ID}" \
  --msg-id "hb_auth_msg_1" \
  --caps-sha256 "${CAPS_SHA}" \
  --auth-alg hmac-sha256-cbor \
  --auth-kid "${KID_NODE}" \
  --auth-seq 1 \
  --hmac-secret "${SECRET_NODE}" | post_cbor_expect_ok

HB_TS="$(python3 - <<'PY'
import time
print(int(time.time() * 1000))
PY
)"

"${ENCODER_BIN}" \
  --type NODE_HEARTBEAT \
  --node-id "${NODE_ID}" \
  --msg-id "hb_auth_msg_2" \
  --ts-utc-ms "${HB_TS}" \
  --health-ok 1 \
  --battery-pct 87.0 \
  --rssi -55.0 \
  --auth-alg hmac-sha256-cbor \
  --auth-kid "${KID_NODE}" \
  --auth-seq 2 \
  --hmac-secret "${SECRET_NODE}" | post_cbor_expect_ok

node_json="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/node?node_id=${NODE_ID}")"
python3 - <<PY
import json, sys
obj = json.loads(r'''${node_json}''')
if not obj.get("ok"):
  print("edge node get failed", obj, file=sys.stderr)
  raise SystemExit(1)
n = obj.get("node") or {}
if n.get("node_id") != "${NODE_ID}":
  print("unexpected node_id", n, file=sys.stderr)
  raise SystemExit(1)
if int(n.get("last_heartbeat_utc_ms") or 0) != int("${HB_TS}"):
  print("expected last_heartbeat_utc_ms", "${HB_TS}", "got", n.get("last_heartbeat_utc_ms"), file=sys.stderr)
  raise SystemExit(1)
h = n.get("health") or {}
if h.get("ok") is not True:
  print("expected health.ok true", h, file=sys.stderr)
  raise SystemExit(1)
if abs(float(h.get("battery_pct") or 0.0) - 87.0) > 1e-9:
  print("expected health.battery_pct 87.0", h, file=sys.stderr)
  raise SystemExit(1)
if abs(float(h.get("rssi") or 0.0) - (-55.0)) > 1e-9:
  print("expected health.rssi -55.0", h, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_edge_heartbeat_auth_hmac_cbor_wire_smoke OK"
