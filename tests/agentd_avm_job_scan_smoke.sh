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

HOST="127.0.0.1"
PORT="$(agentd_smoke_pick_port)"
AUTH_TOKEN="agentd_avm_job_scan_smoke_token"

trap agentd_smoke_stop EXIT

LOG_DIR="$(agentd_smoke_log_dir)"
TMP_DIR="${LOG_DIR}/agentd_avm_job_scan_smoke_${PORT}.tmp"
mkdir -p "${TMP_DIR}"

AVM_STUB_BIN="${TMP_DIR}/avm_stub.sh"
cat > "${AVM_STUB_BIN}" <<'SH'
#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" != "--print-job-json" ]]; then
  echo "unexpected argv (need --print-job-json <file>)" >&2
  exit 2
fi

f="${2:-}"
if [[ -z "${f}" || ! -f "${f}" ]]; then
  echo "missing file" >&2
  exit 2
fi

sz="$(python3 -c 'import os,sys; print(os.path.getsize(sys.argv[1]))' "${f}")"

printf '{"schema":"avm.job.v7","program_hash_sha256":"sha256:stub","policy_hash_sha256":"sha256:stub","input_hash_sha256":"sha256:stub","exec_hash_sha256":"sha256:stub","job_hash_sha256":"sha256:stub","file_size":%s}\n' "${sz}"
SH
chmod +x "${AVM_STUB_BIN}"

export AGENTD_AVM_BIN="${AVM_STUB_BIN}"

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "agentd_avm_job_scan_smoke" \
  --auth-token "${AUTH_TOKEN}" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

obc_bytes="hello_avm_obc"
obc_b64="$(python3 - <<PY
import base64
print(base64.b64encode(b"${obc_bytes}").decode("ascii"))
PY
)"

resp="$(curl -fsS --noproxy "*" --max-time 5 \
  -H "Authorization: Bearer ${AUTH_TOKEN}" \
  -H "Content-Type: application/json" \
  -d "{\"obc_base64\":\"${obc_b64}\"}" \
  "${DAEMON_URL}/api/v1/avm/job_scan")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp}''')
if not obj.get("ok"):
  print(obj, file=sys.stderr)
  raise SystemExit(1)
job = obj.get("job") or {}
if job.get("schema") != "avm.job.v7":
  print("unexpected job.schema:", job.get("schema"), file=sys.stderr)
  raise SystemExit(1)
if job.get("file_size") != len(b"${obc_bytes}"):
  print("unexpected file_size:", job.get("file_size"), file=sys.stderr)
  raise SystemExit(1)
if obj.get("exit_code") != 0:
  print("unexpected exit_code:", obj.get("exit_code"), file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_avm_job_scan_smoke OK"
