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

trap agentd_smoke_stop EXIT

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "agentd_tools_policy_smoke" \
  --tools basic \
  --no-yolo

agentd_smoke_wait_health "${DAEMON_URL}"

# 1) tools endpoint should reject escalating to host.
resp_tools="$(curl -sS --noproxy "*" --max-time 5 -w "\n%{http_code}" "${DAEMON_URL}/api/v1/tools?tools=host")"
body_tools="${resp_tools%$'\n'*}"
status_tools="${resp_tools##*$'\n'}"
if [[ "${status_tools}" != "400" ]]; then
  echo "expected HTTP 400 for tools escalation, got ${status_tools}" >&2
  exit 1
fi
python3 - <<PY
import json, sys
obj = json.loads(r'''${body_tools}''')
err = obj.get("error", "")
if "tools request exceeds daemon tools policy" not in err:
  print("unexpected error:", err, file=sys.stderr)
  raise SystemExit(1)
PY

# 2) run endpoint should reject escalating to host.
resp_run="$(curl -sS --noproxy "*" --max-time 10 -w "\n%{http_code}" \
  -H "Content-Type: application/json" \
  -d '{"prompt":"hi","tools":"host","max_chars":2000,"keep_last":8}' \
  "${DAEMON_URL}/api/v1/run")"
body_run="${resp_run%$'\n'*}"
status_run="${resp_run##*$'\n'}"
if [[ "${status_run}" != "400" ]]; then
  echo "expected HTTP 400 for run tools escalation, got ${status_run}" >&2
  exit 1
fi
python3 - <<PY
import json, sys
obj = json.loads(r'''${body_run}''')
err = obj.get("error", "")
if "tools request exceeds daemon tools policy" not in err:
  print("unexpected error:", err, file=sys.stderr)
  raise SystemExit(1)
if obj.get("rpc_status") != 400:
  print("unexpected rpc_status:", obj.get("rpc_status"), file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_tools_policy_smoke OK"
