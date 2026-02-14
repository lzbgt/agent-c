#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

ts="$(date +%Y%m%d_%H%M%S)"
log_dir="${ROOT}/build/ota_continuity"
mkdir -p "${log_dir}"

agentd_log="${log_dir}/agentd_${ts}.log"
ota_log="${log_dir}/ota_${ts}.log"
state_dir="${log_dir}/state_${ts}"

port="${AGENTD_OTA_TEST_PORT:-18123}"
base_url="http://127.0.0.1:${port}"

if [[ ! -x "${ROOT}/build/agentd" ]]; then
  echo "[ota-smoke] build/agentd missing; run tools/verify.sh first" >&2
  exit 2
fi

mkdir -p "${state_dir}"
agentd_bin="${state_dir}/agentd"
cp "${ROOT}/build/agentd" "${agentd_bin}"

cleanup() {
  set +e
  if [[ -n "${AGENTD_PID:-}" ]]; then kill "${AGENTD_PID}" >/dev/null 2>&1 || true; fi
}
trap cleanup EXIT

wait_http() {
  local url="$1"
  for _ in {1..120}; do
    if curl -fsS "${url}" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.25
  done
  return 1
}

export AGENTD_OTA_ENABLE=1
export AGENTD_OTA_COMMAND="${ROOT}/tools/agentd_ota_apply.sh"
export AGENTD_OTA_TARGET_BIN="${agentd_bin}"
export AGENTD_OTA_RESTART=signal
export AGENTD_OTA_COMMAND_TIMEOUT_MS=120000
export AGENTD_OTA_DRAIN_TIMEOUT_MS=1000

echo "[ota-smoke] starting agentd (log: ${agentd_log})"
"${agentd_bin}" --state-dir "${state_dir}" --host 127.0.0.1 --port "${port}" >"${agentd_log}" 2>&1 &
AGENTD_PID="$!"

if ! wait_http "${base_url}/api/v1/health"; then
  echo "[ota-smoke] agentd failed to start on ${base_url}" >&2
  exit 3
fi

trace_id="ota_smoke_${ts}"
submit_json="$(cat <<JSON
{"trace_id":"${trace_id}","tasks":[{"task_id":"delay_1","kind":"delay","delay_ms":12000}]}
JSON
)"
submit_resp="$(curl -fsS -X POST "${base_url}/api/v1/workflow/submit" -H "Content-Type: application/json" -d "${submit_json}")"
workflow_id="$(SUBMIT_RESP="${submit_resp}" python3 - <<'PY'
import json,os
try:
    obj=json.loads(os.environ.get("SUBMIT_RESP","") or "{}")
    print(obj.get("workflow_id",""))
except Exception:
    print("")
PY
)"
if [[ -z "${workflow_id}" ]]; then
  echo "[ota-smoke] failed to submit workflow: ${submit_resp}" >&2
  exit 4
fi

ota_url="file://${agentd_bin}"
ota_json="$(cat <<JSON
{"url":"${ota_url}","reason":"ota continuity smoke","drain_timeout_ms":500}
JSON
)"
echo "[ota-smoke] requesting OTA update (log: ${ota_log})"
curl -fsS -X POST "${base_url}/api/v1/ota/update" -H "Content-Type: application/json" -d "${ota_json}" >"${ota_log}" 2>&1 || {
  echo "[ota-smoke] ota update failed (see ${ota_log})" >&2
  exit 5
}

echo "[ota-smoke] waiting for agentd to exit..."
for _ in {1..120}; do
  if ! kill -0 "${AGENTD_PID}" >/dev/null 2>&1; then
    break
  fi
  sleep 0.25
done

echo "[ota-smoke] restarting agentd"
"${agentd_bin}" --state-dir "${state_dir}" --host 127.0.0.1 --port "${port}" >>"${agentd_log}" 2>&1 &
AGENTD_PID="$!"

if ! wait_http "${base_url}/api/v1/health"; then
  echo "[ota-smoke] agentd failed to restart on ${base_url}" >&2
  exit 6
fi

echo "[ota-smoke] waiting for workflow to complete..."
deadline=$((SECONDS + 90))
while (( SECONDS < deadline )); do
  wf_resp="$(curl -fsS "${base_url}/api/v1/workflow?workflow_id=${workflow_id}&include_tasks=1")"
  wf_status="$(WF_RESP="${wf_resp}" python3 - <<'PY'
import json,os
try:
    obj=json.loads(os.environ.get("WF_RESP","") or "{}")
    wf=obj.get("workflow",{}) or {}
    print(wf.get("status",""))
except Exception:
    print("")
PY
)"
  if [[ "${wf_status}" == "done" ]]; then
    echo "[ota-smoke] OK (workflow done after restart)"
    exit 0
  fi
  if [[ "${wf_status}" == "error" || "${wf_status}" == "cancelled" ]]; then
    echo "[ota-smoke] workflow failed after restart: ${wf_resp}" >&2
    exit 7
  fi
  sleep 0.5
done

echo "[ota-smoke] timeout waiting for workflow completion" >&2
exit 8
