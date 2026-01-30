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

SESSION_ID="agentd_session_client_events_smoke_$(date +%s)_$RANDOM"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_session_client_events_smoke" \
  --tools host \
  --no-yolo

agentd_smoke_wait_health "${DAEMON_URL}"

# Post a UI event (append_to_session=false: should still land in <session>.client_events.jsonl)
curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${SESSION_ID}",
  "type": "smoke_client_event",
  "data": {"path":"x.wav","k":"v"},
  "append_to_session": False
}))
PY
)" \
  "${DAEMON_URL}/api/v1/session/ui_event" >/dev/null

resp="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/session/client_events?session_id=$(python3 -c 'import urllib.parse; print(urllib.parse.quote("""'${SESSION_ID}'"""))')&max_bytes=1048576")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp}''')
if not obj.get("ok"):
  print("session/client_events failed:", obj, file=sys.stderr)
  raise SystemExit(1)
events = obj.get("events")
if not isinstance(events, list):
  print("expected events list", type(events), file=sys.stderr)
  raise SystemExit(1)
if not any(isinstance(e, dict) and e.get("type") == "smoke_client_event" for e in events):
  print("expected smoke_client_event in events; got:", events, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_session_client_events_smoke OK"
