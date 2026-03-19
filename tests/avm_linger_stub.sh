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
    --print-job-json|--print-policy-json|--inspect-json|--verify-strict|--print-trace-hash)
      if [[ -z "${mode}" ]]; then mode="${a}"; fi
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

cleanup() {
  exit 0
}
trap cleanup TERM INT

case "${mode}" in
  --print-job-json)
    printf '{"schema":"avm.job.v7","program_hash_sha256":"sha256:linger","policy_hash_sha256":"sha256:linger","input_hash_sha256":"sha256:linger","exec_hash_sha256":"sha256:linger","job_hash_sha256":"sha256:linger"}\n'
    ;;
  --print-policy-json)
    printf '{"schema":"avm.policy.v1","policy_hash_sha256":"sha256:linger","used_domains_mask":"0x0","pairs":[]}\n'
    ;;
  --inspect-json)
    printf '{"schema":"avm.inspect.v1","program_hash_sha256":"sha256:linger","capabilities":{"schema":"avm.policy.v1","policy_hash_sha256":"sha256:linger"}}\n'
    ;;
  --verify-strict)
    echo "OK"
    ;;
  --print-run-json)
    echo '{"schema":"avm.run.v1","exit_code":0,"gas_executed":1,"wall_elapsed_ns":1,"has_result":true,"result_type":"string","result":"linger"}'
    ;;
  --print-trace-hash)
    echo "TRACE_HASH lingertracehash"
    ;;
  *)
    echo "unexpected argv (mode not found)" >&2
    exit 2
    ;;
esac

if [[ -n "${AGENTD_AVM_LINGER_MARKER:-}" ]]; then
  : > "${AGENTD_AVM_LINGER_MARKER}"
fi

while true; do
  sleep 1
done
