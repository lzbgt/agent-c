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

SESSION_ID="agentd_session_clients_smoke_$(date +%s)_$RANDOM"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_session_clients_smoke" \
  --tools host \
  --no-yolo

agentd_smoke_wait_health "${DAEMON_URL}"

post_event() {
  local client_id="$1"
  local instance_id="$2"
  local type="$3"
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/json" \
    -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${SESSION_ID}",
  "type": "${type}",
  "client": {"id": "${client_id}", "kind": "smoke", "instance_id": "${instance_id}"},
  "data": {"k":"v"},
  "append_to_session": False
}))
PY
)" \
    "${DAEMON_URL}/api/v1/session/client_event" >/dev/null
}

post_event "webui" "tabA" "smoke_1"
sleep 0.05
post_event "slack" "thread1" "smoke_2"

resp="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/session/clients?session_id=${SESSION_ID}&max_bytes=1048576&include_rotated=1")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp}''')
if not obj.get("ok"):
  print("clients endpoint failed:", obj, file=sys.stderr)
  raise SystemExit(1)
clients = obj.get("clients") or []
ids = set()
for c in clients:
  if isinstance(c, dict) and isinstance(c.get("id"), str):
    ids.add(c["id"])
if "webui" not in ids or "slack" not in ids:
  print("expected client ids not found:", ids, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_session_clients_smoke OK"

