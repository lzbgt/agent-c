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
NAME="agentd_blob_smoke"

DB_PATH="${LOG_DIR}/${NAME}_${PORT_DAEMON}.sqlite"
STATE_DIR="${LOG_DIR}/${NAME}_${PORT_DAEMON}.state"

cleanup() {
  agentd_smoke_stop
  rm -f "${DB_PATH}" "${DB_PATH}-wal" "${DB_PATH}-shm" >/dev/null 2>&1 || true
  rm -rf "${STATE_DIR}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}" \
  --db-path "${DB_PATH}" \
  --state-dir "${STATE_DIR}"

agentd_smoke_wait_health "${DAEMON_URL}"

payload="hello blob"
b64="$(python3 - <<PY
import base64
print(base64.b64encode(b"${payload}").decode("ascii"))
PY
)"

upload_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "data_base64": "${b64}",
  "mime": "text/plain",
  "retain": True
}))
PY
)" \
  "${DAEMON_URL}/api/v1/blob/upload")"

BLOB_ID="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${upload_resp}''')
if not obj.get("ok"):
  print("upload failed:", obj, file=sys.stderr)
  raise SystemExit(1)
bid = obj.get("blob_id")
if not isinstance(bid, str) or not bid.startswith("sha256:"):
  print("invalid blob_id:", bid, file=sys.stderr)
  raise SystemExit(1)
size = obj.get("size_bytes")
if int(size or 0) != len(b"${payload}"):
  print("unexpected size:", size, file=sys.stderr)
  raise SystemExit(1)
print(bid)
PY
)"

meta_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/blob/meta?blob_id=${BLOB_ID}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${meta_resp}''')
if not obj.get("ok"):
  print("meta failed:", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("blob_id") != "${BLOB_ID}":
  print("unexpected blob_id:", obj.get("blob_id"), file=sys.stderr)
  raise SystemExit(1)
if int(obj.get("ref_count") or 0) < 1:
  print("expected ref_count >= 1", obj, file=sys.stderr)
  raise SystemExit(1)
PY

blob_bytes="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/blob?blob_id=${BLOB_ID}")"
if [[ "${blob_bytes}" != "${payload}" ]]; then
  echo "unexpected blob bytes: ${blob_bytes}" >&2
  exit 1
fi

range_bytes="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Range: bytes=0-4" \
  "${DAEMON_URL}/api/v1/blob?blob_id=${BLOB_ID}")"
if [[ "${range_bytes}" != "hello" ]]; then
  echo "unexpected range bytes: ${range_bytes}" >&2
  exit 1
fi

retain_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "{\"blob_id\":\"${BLOB_ID}\",\"delta\":-1}" \
  "${DAEMON_URL}/api/v1/blob/retain")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${retain_resp}''')
if not obj.get("ok"):
  print("retain failed:", obj, file=sys.stderr)
  raise SystemExit(1)
if int(obj.get("ref_count") or 0) != 0:
  print("expected ref_count 0:", obj, file=sys.stderr)
  raise SystemExit(1)
PY

gc_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "{\"min_age_ms\":0,\"max_rows\":100}" \
  "${DAEMON_URL}/api/v1/blob/gc")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${gc_resp}''')
if not obj.get("ok"):
  print("gc failed:", obj, file=sys.stderr)
  raise SystemExit(1)
deleted = obj.get("deleted") or []
if not any(isinstance(d, dict) and d.get("blob_id") == "${BLOB_ID}" for d in deleted):
  print("expected blob_id in deleted list", obj, file=sys.stderr)
  raise SystemExit(1)
PY

meta_status="$(curl -sS -o /dev/null -w "%{http_code}" --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/blob/meta?blob_id=${BLOB_ID}")"
if [[ "${meta_status}" != "404" ]]; then
  echo "expected 404 after gc; got ${meta_status}" >&2
  exit 1
fi

hex="${BLOB_ID#sha256:}"
blob_path="${STATE_DIR}/blobs/sha256/${hex:0:2}/${hex:2:2}/${hex}"
if [[ -f "${blob_path}" ]]; then
  echo "expected blob file to be removed: ${blob_path}" >&2
  exit 1
fi

tier_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "{\"dry_run\":true}" \
  "${DAEMON_URL}/api/v1/blob/tier/enforce")"
python3 - <<PY
import json, sys
obj = json.loads(r'''${tier_resp}''')
if not obj.get("ok"):
  print("tier enforce failed:", obj, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_blob_smoke OK"
