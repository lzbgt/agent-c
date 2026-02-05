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

CBOR_CHECK_BIN="${2:-}"

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

HOST="127.0.0.1"
PORT="$(agentd_smoke_pick_port)"

trap agentd_smoke_stop EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "agentd_edge_outbox_cbor_smoke" \
  --tools none
agentd_smoke_wait_health "${DAEMON_URL}"

NODE_ID="node_outbox_cbor_1"
CAPS="sha256:$(python3 - <<'PY'
print("b"*64)
PY
)"

# Register node via JSON so the platform enqueues a PLATFORM_CAPS_REQ.
hello="$(python3 - <<PY
import json, time
print(json.dumps({
  "msg_id": "json_msg_1",
  "ts_utc_ms": int(time.time()*1000),
  "type": "NODE_HELLO",
  "from": "node:${NODE_ID}",
  "to": "platform",
  "body": {"node_id":"${NODE_ID}","model":"esp32","fw_git_sha":"deadbeef","caps_sha256":"${CAPS}"},
}))
PY
)"
curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${hello}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

hdr="${LOG_DIR}/agentd_edge_outbox_cbor_smoke.headers"
body="${LOG_DIR}/agentd_edge_outbox_cbor_smoke.body.cbor"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Accept: application/cbor" \
  -D "${hdr}" \
  -o "${body}" \
  "${DAEMON_URL}/api/v1/edge/outbox?node_id=${NODE_ID}&cursor=0&limit=10" >/dev/null

python3 - <<PY
import re, sys
hdr = open(r'''${hdr}''','rb').read().decode('latin1', errors='replace')
if not re.search(r'(?im)^content-type:\\s*application/cbor\\b', hdr):
  print("expected Content-Type: application/cbor, got headers:", hdr, file=sys.stderr)
  raise SystemExit(1)
PY

if [[ -n "${CBOR_CHECK_BIN}" && -x "${CBOR_CHECK_BIN}" ]]; then
  "${CBOR_CHECK_BIN}" --file "${body}" --node-id "${NODE_ID}" --expect-type "PLATFORM_CAPS_REQ"
else
  # Fallback: use a tiny Python CBOR decoder (kept for portability if the helper binary is not present).
  python3 - <<PY
import sys

data = open(r'''${body}''','rb').read()

class CborErr(Exception): pass

def read_u(data, i, n):
  if i+n > len(data): raise CborErr("truncated")
  return data[i:i+n], i+n

def read_ai(i, ai):
  if ai < 24: return ai, i
  if ai == 24:
    b, i = read_u(data, i, 1)
    return b[0], i
  if ai == 25:
    b, i = read_u(data, i, 2)
    return int.from_bytes(b, 'big'), i
  if ai == 26:
    b, i = read_u(data, i, 4)
    return int.from_bytes(b, 'big'), i
  if ai == 27:
    b, i = read_u(data, i, 8)
    return int.from_bytes(b, 'big'), i
  if ai == 31:
    raise CborErr("indefinite not supported")
  raise CborErr("reserved ai")

def decode(i=0, depth=0):
  if depth > 64: raise CborErr("depth")
  b, i = read_u(data, i, 1)
  ib = b[0]
  major = (ib >> 5) & 7
  ai = ib & 31
  if major == 0:
    n, i = read_ai(i, ai)
    return n, i
  if major == 1:
    n, i = read_ai(i, ai)
    return -1 - n, i
  if major == 3:
    n, i = read_ai(i, ai)
    s, i = read_u(data, i, n)
    return s.decode('utf-8', errors='strict'), i
  if major == 4:
    n, i = read_ai(i, ai)
    arr = []
    for _ in range(n):
      v, i = decode(i, depth+1)
      arr.append(v)
    return arr, i
  if major == 5:
    n, i = read_ai(i, ai)
    obj = {}
    for _ in range(n):
      k, i = decode(i, depth+1)
      if not isinstance(k, str): raise CborErr("non-text key")
      v, i = decode(i, depth+1)
      obj[k] = v
    return obj, i
  if major == 7:
    if ai == 20: return False, i
    if ai == 21: return True, i
    if ai == 22: return None, i
    if ai == 23: return None, i
    raise CborErr("unsupported simple/float")
  raise CborErr("unsupported major")

obj, i = decode(0, 0)
if i != len(data):
  raise CborErr("trailing bytes")
if not isinstance(obj, dict) or not obj.get("ok"):
  print("unexpected outbox object", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("node_id") != "${NODE_ID}":
  print("unexpected node_id", obj.get("node_id"), file=sys.stderr)
  raise SystemExit(1)
msgs = obj.get("messages") or []
types = []
for row in msgs:
  if not isinstance(row, dict): continue
  env = row.get("msg") or {}
  if isinstance(env, dict): types.append(env.get("type"))
if "PLATFORM_CAPS_REQ" not in types:
  print("expected PLATFORM_CAPS_REQ in CBOR outbox, got", types, file=sys.stderr)
  raise SystemExit(1)
PY
fi

echo "agentd_edge_outbox_cbor_smoke OK"
