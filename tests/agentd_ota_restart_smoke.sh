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

PORT="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"
NAME="agentd_ota_restart_smoke"

cleanup() {
  unset AGENTD_OTA_RESTART
  unset AGENTD_OTA_SERVICE
  unset AGENTD_OTA_RESTART_DRY_RUN
  agentd_smoke_stop
}
trap cleanup EXIT

export AGENTD_OTA_RESTART=systemd
export AGENTD_OTA_SERVICE=agentd-smoke
export AGENTD_OTA_RESTART_DRY_RUN=1

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "${NAME}" \
  --tools none \
  --no-yolo \
  --ota-enable \
  --ota-command /bin/true \
  --ota-drain-timeout-ms 1000

agentd_smoke_wait_health "${DAEMON_URL}"

status_json="$(curl -fsS --noproxy "*" --max-time 10 "${DAEMON_URL}/api/v1/ota/status")"
python3 - <<PY
import json, sys
obj = json.loads(r'''${status_json}''')
restart = obj.get("restart") or {}
if restart.get("enabled") is not True:
    print("restart should be enabled", obj, file=sys.stderr)
    raise SystemExit(1)
if restart.get("safe_boundary") != "agentd_supervisor_restart_drain":
    print("bad restart safe boundary", obj, file=sys.stderr)
    raise SystemExit(1)
if restart.get("method") != "systemd" or restart.get("service") != "agentd-smoke":
    print("bad supervisor identity", obj, file=sys.stderr)
    raise SystemExit(1)
if restart.get("dry_run") is not True:
    print("restart smoke must run in dry-run mode", obj, file=sys.stderr)
    raise SystemExit(1)
PY

request_json='{"reason":"ota restart smoke","idempotency_key":"restart-smoke-key","drain_timeout_ms":5000}'
first_json="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${request_json}" \
  "${DAEMON_URL}/api/v1/ota/restart")"
second_json="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${request_json}" \
  "${DAEMON_URL}/api/v1/ota/restart")"

python3 - <<PY
import json, sys
first = json.loads(r'''${first_json}''')
second = json.loads(r'''${second_json}''')
if first.get("ok") is not True or first.get("operation") != "restart":
    print("restart not accepted", first, file=sys.stderr)
    raise SystemExit(1)
if first.get("dry_run") is not True:
    print("restart response lost dry_run", first, file=sys.stderr)
    raise SystemExit(1)
if second.get("ok") is not True or second.get("idempotent_replay") is not True:
    print("restart idempotency replay failed", second, file=sys.stderr)
    raise SystemExit(1)
if second.get("ota_id") != first.get("ota_id"):
    print("restart idempotency returned different ota_id", first, second, file=sys.stderr)
    raise SystemExit(1)
PY

echo "agentd_ota_restart_smoke OK"
