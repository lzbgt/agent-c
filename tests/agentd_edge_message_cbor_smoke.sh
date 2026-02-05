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

HOST="127.0.0.1"
PORT="$(agentd_smoke_pick_port)"

trap agentd_smoke_stop EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "agentd_edge_message_cbor_smoke" \
  --tools none
agentd_smoke_wait_health "${DAEMON_URL}"

NODE_ID="node_cbor_1"
CAPS="sha256:$(python3 - <<'PY'
print("a"*64)
PY
)"
MSG_ID="cbor_msg_1"

# Build a CBOR-encoded UM-BMP NODE_HELLO envelope and POST it with Content-Type: application/cbor.
resp="$(
  python3 - <<PY | curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/cbor" \
    --data-binary @- \
    "${DAEMON_URL}/api/v1/edge/message"
import struct, sys, time

def enc_uint(u):
  if u < 24: return bytes([0x00 | u])
  if u < 256: return bytes([0x18, u])
  if u < 65536: return bytes([0x19]) + struct.pack(">H", u)
  if u < (1<<32): return bytes([0x1a]) + struct.pack(">I", u)
  return bytes([0x1b]) + struct.pack(">Q", u)

def enc_text(s):
  b = s.encode("utf-8")
  n = len(b)
  if n < 24: hdr = bytes([0x60 | n])
  elif n < 256: hdr = bytes([0x78, n])
  elif n < 65536: hdr = bytes([0x79]) + struct.pack(">H", n)
  else: raise SystemExit("text too long")
  return hdr + b

def enc_bool(v):
  return bytes([0xf5 if v else 0xf4])

def enc_null():
  return bytes([0xf6])

def enc_map(m):
  items = []
  for k, v in m.items():
    items.append(enc_text(k))
    items.append(v)
  n = len(m)
  if n < 24: hdr = bytes([0xa0 | n])
  elif n < 256: hdr = bytes([0xb8, n])
  else: raise SystemExit("map too large")
  return hdr + b"".join(items)

ts = int(time.time() * 1000)

body = enc_map({
  "node_id": enc_text("${NODE_ID}"),
  "model": enc_text("esp32"),
  "fw_git_sha": enc_text("deadbeef"),
  "caps_sha256": enc_text("${CAPS}"),
})

env = enc_map({
  "msg_id": enc_text("${MSG_ID}"),
  "ts_utc_ms": enc_uint(ts),
  "type": enc_text("NODE_HELLO"),
  "from": enc_text("node:${NODE_ID}"),
  "to": enc_text("platform"),
  "body": body,
  "trace": enc_null(),
})

sys.stdout.buffer.write(env)
PY
)"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp}''')
if not obj.get("ok"):
  print("expected ok true", obj, file=sys.stderr)
  raise SystemExit(1)
PY

# Verify node is registered.
nodes="$(curl -fsS --noproxy "*" --max-time 10 "${DAEMON_URL}/api/v1/edge/nodes")"
python3 - <<PY
import json, sys
obj = json.loads(r'''${nodes}''')
if not obj.get("ok"):
  print("nodes failed", obj, file=sys.stderr)
  raise SystemExit(1)
arr = obj.get("nodes") or []
if not any(isinstance(n, dict) and n.get("node_id") == "${NODE_ID}" for n in arr):
  print("missing node_id in nodes", arr, file=sys.stderr)
  raise SystemExit(1)
PY

# Verify outbox got a PLATFORM_CAPS_REQ (since caps_sha256 was set and manifest is not known yet).
outbox="$(curl -fsS --noproxy "*" --max-time 10 "${DAEMON_URL}/api/v1/edge/outbox?node_id=${NODE_ID}&cursor=0&limit=10")"
python3 - <<PY
import json, sys
obj = json.loads(r'''${outbox}''')
if not obj.get("ok"):
  print("outbox failed", obj, file=sys.stderr)
  raise SystemExit(1)
msgs = obj.get("messages") or []
types = []
for m in msgs:
  if not isinstance(m, dict): continue
  env = m.get("msg") or {}
  if isinstance(env, dict): types.append(env.get("type"))
if "PLATFORM_CAPS_REQ" not in types:
  print("expected PLATFORM_CAPS_REQ in outbox, got", types, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_edge_message_cbor_smoke OK"
