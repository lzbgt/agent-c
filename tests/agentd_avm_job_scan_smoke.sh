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

HOME_ROOT="${TMP_DIR}/home"
ALLOW_ROOT="${TMP_DIR}/allow_root"
OUTSIDE_ROOT="${TMP_DIR}/outside_root"
mkdir -p "${HOME_ROOT}/.config/agent" "${ALLOW_ROOT}/allowed" "${OUTSIDE_ROOT}/blocked"
cat > "${HOME_ROOT}/.config/agent/mount-allowlist.json" <<JSON
{
  "allowed_roots": [
    { "path": "${ALLOW_ROOT}", "readonly": false }
  ],
  "blocked_patterns": [],
  "non_main_readonly": true
}
JSON

export HOME="${HOME_ROOT}"
export EXPECTED_ALLOWED_HOST="${ALLOW_ROOT}/allowed"
export EXPECTED_ALLOWED_CONTAINER="/workspace/extra/allowed"
export EXPECTED_ALLOWED_MATCHED_ROOT="${ALLOW_ROOT}"
export EXPECTED_ALLOWED_READONLY="0"

AVM_STUB_BIN="${TMP_DIR}/avm_stub.sh"
cat > "${AVM_STUB_BIN}" <<'SH'
#!/usr/bin/env bash
set -euo pipefail

mode=""
file=""

args=("$@")
i=0
while [[ $i -lt ${#args[@]} ]]; do
  a="${args[$i]}"
  case "${a}" in
    --timeout-ms|--allow-domains)
      i=$((i + 2))
      continue
      ;;
    --capsule|--print-result-hash|--print-state-hash)
      i=$((i + 1))
      continue
      ;;
    --print-run-json)
      mode="--print-run-json"
      i=$((i + 1))
      continue
      ;;
    --print-job-json|--print-policy-json|--inspect-json|--verify-strict)
      if [[ -z "${mode}" ]]; then mode="${a}"; fi
      i=$((i + 1))
      continue
      ;;
    --print-trace-hash)
      if [[ -z "${mode}" ]]; then mode="--print-trace-hash"; fi
      i=$((i + 1))
      continue
      ;;
    *)
      if [[ -z "${file}" && "${a}" != -* ]]; then
        file="${a}"
      fi
      i=$((i + 1))
      continue
      ;;
  esac
done

if [[ -z "${file}" || ! -f "${file}" ]]; then
  echo "missing file" >&2
  exit 2
fi

sz="$(python3 -c 'import os,sys; print(os.path.getsize(sys.argv[1]))' "${file}")"

case "${mode}" in
  --print-job-json)
    printf '{"schema":"avm.job.v7","program_hash_sha256":"sha256:stub","policy_hash_sha256":"sha256:stub","input_hash_sha256":"sha256:stub","exec_hash_sha256":"sha256:stub","job_hash_sha256":"sha256:stub","file_size":%s}\n' "${sz}"
    ;;
  --print-policy-json)
    printf '{"schema":"avm.policy.v1","policy_hash_sha256":"sha256:stub","used_domains_mask":"0x0","pairs":[],"file_size":%s}\n' "${sz}"
    ;;
  --inspect-json)
    printf '{"schema":"avm.inspect.v1","program_hash_sha256":"sha256:stub","file_size":%s,"capabilities":{"schema":"avm.policy.v1","policy_hash_sha256":"sha256:stub"}}\n' "${sz}"
    ;;
  --verify-strict)
    echo "OK"
    ;;
  --print-run-json)
    mount_count="${AGENTD_AVM_MOUNT_COUNT:-0}"
    if [[ "${mount_count}" != "0" ]]; then
      if [[ "${mount_count}" != "1" ]]; then
        echo "unexpected mount_count=${mount_count}" >&2
        exit 2
      fi
      if [[ "${AGENTD_AVM_MOUNT_0_HOST_PATH:-}" != "${EXPECTED_ALLOWED_HOST:-}" ]]; then
        echo "unexpected host path ${AGENTD_AVM_MOUNT_0_HOST_PATH:-}" >&2
        exit 2
      fi
      if [[ "${AGENTD_AVM_MOUNT_0_CONTAINER_PATH:-}" != "${EXPECTED_ALLOWED_CONTAINER:-}" ]]; then
        echo "unexpected container path ${AGENTD_AVM_MOUNT_0_CONTAINER_PATH:-}" >&2
        exit 2
      fi
      if [[ "${AGENTD_AVM_MOUNT_0_MATCHED_ROOT:-}" != "${EXPECTED_ALLOWED_MATCHED_ROOT:-}" ]]; then
        echo "unexpected matched root ${AGENTD_AVM_MOUNT_0_MATCHED_ROOT:-}" >&2
        exit 2
      fi
      if [[ "${AGENTD_AVM_MOUNT_0_READONLY:-}" != "${EXPECTED_ALLOWED_READONLY:-}" ]]; then
        echo "unexpected readonly ${AGENTD_AVM_MOUNT_0_READONLY:-}" >&2
        exit 2
      fi
      python3 - <<'PY'
import json, os, sys
mounts = json.loads(os.environ.get("AGENTD_AVM_MOUNTS_JSON", "[]"))
if len(mounts) != 1:
  print("unexpected mounts json", mounts, file=sys.stderr)
  raise SystemExit(1)
mount = mounts[0]
expected = {
  "host_path": os.environ["EXPECTED_ALLOWED_HOST"],
  "container_path": os.environ["EXPECTED_ALLOWED_CONTAINER"],
  "matched_root": os.environ["EXPECTED_ALLOWED_MATCHED_ROOT"],
  "readonly": False,
  "is_main": True,
}
for key, value in expected.items():
  if mount.get(key) != value:
    print(f"unexpected mount field {key}: {mount.get(key)!r}", file=sys.stderr)
    raise SystemExit(1)
PY
    fi
    echo '{"schema":"avm.run.v1","exit_code":0,"gas_executed":10,"wall_elapsed_ns":1000,"has_result":true,"result_type":"string","result":"ok"}'
    echo "RESULT_HASH stubresulthash"
    echo "TRACE_HASH stubtracehash"
    echo "STATE_HASH stubstatehash"
    ;;
  --print-trace-hash)
    echo "TRACE_HASH stubtracehash"
    ;;
  *)
    echo "unexpected argv (mode not found)" >&2
    exit 2
    ;;
esac
SH
chmod +x "${AVM_STUB_BIN}"

export AGENTD_AVM_BIN="${AVM_STUB_BIN}"
export AGENTD_AVM_EXEC=1

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

resp_policy="$(curl -fsS --noproxy "*" --max-time 5 \
  -H "Authorization: Bearer ${AUTH_TOKEN}" \
  -H "Content-Type: application/json" \
  -d "{\"obc_base64\":\"${obc_b64}\"}" \
  "${DAEMON_URL}/api/v1/avm/policy_scan")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp_policy}''')
if not obj.get("ok"):
  print(obj, file=sys.stderr)
  raise SystemExit(1)
pol = obj.get("policy") or {}
if pol.get("schema") != "avm.policy.v1":
  print("unexpected policy.schema:", pol.get("schema"), file=sys.stderr)
  raise SystemExit(1)
if obj.get("exit_code") != 0:
  print("unexpected exit_code:", obj.get("exit_code"), file=sys.stderr)
  raise SystemExit(1)
PY

resp_inspect="$(curl -fsS --noproxy "*" --max-time 5 \
  -H "Authorization: Bearer ${AUTH_TOKEN}" \
  -H "Content-Type: application/json" \
  -d "{\"obc_base64\":\"${obc_b64}\"}" \
  "${DAEMON_URL}/api/v1/avm/inspect")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp_inspect}''')
if not obj.get("ok"):
  print(obj, file=sys.stderr)
  raise SystemExit(1)
ins = obj.get("inspect") or {}
if ins.get("schema") != "avm.inspect.v1":
  print("unexpected inspect.schema:", ins.get("schema"), file=sys.stderr)
  raise SystemExit(1)
if ins.get("file_size") != len(b"${obc_bytes}"):
  print("unexpected inspect.file_size:", ins.get("file_size"), file=sys.stderr)
  raise SystemExit(1)
PY

resp_verify="$(curl -fsS --noproxy "*" --max-time 5 \
  -H "Authorization: Bearer ${AUTH_TOKEN}" \
  -H "Content-Type: application/json" \
  -d "{\"obc_base64\":\"${obc_b64}\"}" \
  "${DAEMON_URL}/api/v1/avm/verify_strict")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp_verify}''')
if not obj.get("ok"):
  print(obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("exit_code") != 0:
  print("unexpected exit_code:", obj.get("exit_code"), file=sys.stderr)
  raise SystemExit(1)
PY

resp_trace="$(curl -fsS --noproxy "*" --max-time 5 \
  -H "Authorization: Bearer ${AUTH_TOKEN}" \
  -H "Content-Type: application/json" \
  -d "{\"obc_base64\":\"${obc_b64}\"}" \
  "${DAEMON_URL}/api/v1/avm/trace_hash")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp_trace}''')
if not obj.get("ok"):
  print(obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("trace_hash") != "stubtracehash":
  print("unexpected trace_hash:", obj.get("trace_hash"), file=sys.stderr)
  raise SystemExit(1)
PY

resp_run="$(curl -fsS --noproxy "*" --max-time 5 \
  -H "Authorization: Bearer ${AUTH_TOKEN}" \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "obc_base64": "${obc_b64}",
  "timeout_ms": 1000,
  "gas": 1000,
  "mem_bytes": 64000,
  "io_bytes": 0,
  "log_bytes": 0,
  "deterministic": True,
  "mounts": [
    {
      "host_path": "${ALLOW_ROOT}/allowed",
      "container_path": "/workspace/extra/allowed",
      "is_main": True,
    }
  ],
}))
PY
)" \
  "${DAEMON_URL}/api/v1/avm/capsule_run")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp_run}''')
run = obj.get("run") or {}
if run.get("schema") != "avm.run.v1":
  print("unexpected run.schema:", run.get("schema"), file=sys.stderr)
  raise SystemExit(1)
if obj.get("result_hash") != "stubresulthash":
  print("unexpected result_hash:", obj.get("result_hash"), file=sys.stderr)
  raise SystemExit(1)
if obj.get("trace_hash") != "stubtracehash":
  print("unexpected trace_hash:", obj.get("trace_hash"), file=sys.stderr)
  raise SystemExit(1)
if obj.get("state_hash") != "stubstatehash":
  print("unexpected state_hash:", obj.get("state_hash"), file=sys.stderr)
  raise SystemExit(1)
mounts = obj.get("mounts") or []
if len(mounts) != 1:
  print("unexpected mounts:", mounts, file=sys.stderr)
  raise SystemExit(1)
mount = mounts[0]
if mount.get("host_path") != "${ALLOW_ROOT}/allowed":
  print("unexpected response host_path:", mount.get("host_path"), file=sys.stderr)
  raise SystemExit(1)
if mount.get("container_path") != "/workspace/extra/allowed":
  print("unexpected response container_path:", mount.get("container_path"), file=sys.stderr)
  raise SystemExit(1)
if mount.get("readonly") is not False:
  print("unexpected response readonly:", mount.get("readonly"), file=sys.stderr)
  raise SystemExit(1)
PY

resp_run_bad_file="${TMP_DIR}/avm_capsule_run_bad_mount.json"
resp_run_bad_status="$(curl -sS --noproxy "*" --max-time 5 \
  -o "${resp_run_bad_file}" \
  -w "%{http_code}" \
  -H "Authorization: Bearer ${AUTH_TOKEN}" \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "obc_base64": "${obc_b64}",
  "timeout_ms": 1000,
  "mounts": [
    {
      "host_path": "${OUTSIDE_ROOT}/blocked",
      "container_path": "/workspace/extra/blocked",
      "is_main": True,
    }
  ],
}))
PY
)" \
  "${DAEMON_URL}/api/v1/avm/capsule_run")"

if [[ "${resp_run_bad_status}" != "403" ]]; then
  echo "expected 403 for blocked mount, got ${resp_run_bad_status}" >&2
  cat "${resp_run_bad_file}" >&2
  exit 1
fi

python3 - <<PY
import json, pathlib, sys
obj = json.loads(pathlib.Path(r'''${resp_run_bad_file}''').read_text())
if obj.get("ok") is not False:
  print("expected ok=false", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("error_kind") != "forbidden":
  print("unexpected error_kind", obj.get("error_kind"), file=sys.stderr)
  raise SystemExit(1)
err = obj.get("error") or ""
if "host_path_outside_roots" not in err:
  print("unexpected error", err, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_avm_job_scan_smoke OK"
