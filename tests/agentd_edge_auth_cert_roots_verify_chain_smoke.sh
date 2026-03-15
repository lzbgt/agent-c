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

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_auth_cert_roots_verify_chain_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

CA_PEM_PATH="${ROOT}/tools/_compose_mtls/ca.pem"
SERVER_CERT_PATH="${ROOT}/tools/_compose_mtls/server.pem"
CLIENT_CERT_PATH="${ROOT}/tools/_compose_mtls/client.pem"

rotate_root() {
  local kid="$1"
  local pem_path="$2"
  local epoch="$3"
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/json" \
    -d "$(python3 - <<PY
import json, pathlib
print(json.dumps({
  "mode": "replace",
  "rotation_epoch": ${epoch},
  "edge_auth_cert_roots_pem": {
    "${kid}": pathlib.Path("${pem_path}").read_text(encoding="utf-8")
  },
}))
PY
)" \
    "${DAEMON_URL}/api/v1/edge/auth/cert_roots/rotate"
}

GOOD_ROTATE="$(rotate_root lab-ca-1 "${CA_PEM_PATH}" 3)"
python3 - <<PY
import json, sys
obj = json.loads(r'''${GOOD_ROTATE}''')
if not obj.get("ok") or obj.get("rotation_epoch") != 3:
  print("good rotate failed", obj, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

verify_cert() {
  local cert_path="$1"
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/json" \
    -d "$(python3 - <<PY
import json, pathlib
print(json.dumps({
  "cert_pem": pathlib.Path("${cert_path}").read_text(encoding="utf-8")
}))
PY
)" \
    "${DAEMON_URL}/api/v1/edge/auth/cert_roots/verify_chain"
}

GOOD_SERVER="$(verify_cert "${SERVER_CERT_PATH}")"
python3 - <<PY
import json, sys
obj = json.loads(r'''${GOOD_SERVER}''')
if not obj.get("ok"):
  print("server verify endpoint failed", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("verified") is not True:
  print("server verify did not pass", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("matched_root_kids") != ["lab-ca-1"]:
  print("unexpected matched root kids", obj.get("matched_root_kids"), file=sys.stderr)
  raise SystemExit(1)
subjects = obj.get("verified_chain_subjects") or []
if len(subjects) < 2:
  print("expected verified chain subjects", subjects, file=sys.stderr)
  raise SystemExit(1)
if not obj.get("leaf_subject") or not obj.get("leaf_issuer") or not obj.get("leaf_sha256"):
  print("missing leaf summary", obj, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

GOOD_CLIENT="$(verify_cert "${CLIENT_CERT_PATH}")"
python3 - <<PY
import json, sys
obj = json.loads(r'''${GOOD_CLIENT}''')
if not obj.get("ok") or obj.get("verified") is not True:
  print("client verify did not pass", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("matched_root_kids") != ["lab-ca-1"]:
  print("unexpected matched root kids", obj.get("matched_root_kids"), file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

BAD_ROTATE="$(rotate_root not-a-ca "${CLIENT_CERT_PATH}" 4)"

python3 - <<PY
import json, sys
obj = json.loads(r'''${BAD_ROTATE}''')
if not obj.get("ok") or obj.get("rotation_epoch") != 4:
  print("bad rotate failed", obj, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

BAD_SERVER="$(verify_cert "${SERVER_CERT_PATH}")"
python3 - <<PY
import json, sys
obj = json.loads(r'''${BAD_SERVER}''')
if not obj.get("ok"):
  print("bad verify endpoint failed", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("verified") is not False:
  print("expected failed verification", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("matched_root_kids") not in ([], None):
  print("unexpected matched root kids on failed verify", obj.get("matched_root_kids"), file=sys.stderr)
  raise SystemExit(1)
if not obj.get("verify_error"):
  print("expected verify_error on failed verify", obj, file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

status_bad_pem="$(
  curl -sS --noproxy "*" -o /dev/null -w '%{http_code}' --max-time 10 \
    -H 'Content-Type: application/json' \
    -d '{"cert_pem":"not-a-cert"}' \
    "${DAEMON_URL}/api/v1/edge/auth/cert_roots/verify_chain"
)"
if [[ "${status_bad_pem}" != "400" ]]; then
  echo "expected 400 for malformed cert_pem, got ${status_bad_pem}" >&2
  exit 1
fi

echo "agentd_edge_auth_cert_roots_verify_chain_smoke OK"
