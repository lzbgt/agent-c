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
SESSION_ID="agentd_session_voice_control_smoke_$(date +%s)_$RANDOM"
AUDIO_DATA_URL="data:audio/wav;base64,UklGRiQAAABXQVZFZm10IBAAAAABAAEAESsAACJWAAACABAAZGF0YQAAAAA="

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_session_voice_control_smoke" \
  --tools host \
  --no-yolo

agentd_smoke_wait_health "${DAEMON_URL}"

play_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${SESSION_ID}",
  "action": "play",
  "id": "voice-audio",
  "url": "${AUDIO_DATA_URL}",
  "muted": True,
  "controls": True,
  "title": "Voice play smoke",
}))
PY
)" \
  "${DAEMON_URL}/api/v1/session/voice_control")"

snapshot_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${SESSION_ID}",
  "action": "snapshot",
  "title": "Voice snapshot smoke",
}))
PY
)" \
  "${DAEMON_URL}/api/v1/session/voice_control")"

pause_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${SESSION_ID}",
  "action": "pause",
  "selector": "#voice-audio",
  "title": "Voice pause smoke",
}))
PY
)" \
  "${DAEMON_URL}/api/v1/session/voice_control")"

mapfile -t ids < <(python3 - <<PY
import json
rows = [
  json.loads(r'''${play_resp}'''),
  json.loads(r'''${snapshot_resp}'''),
  json.loads(r'''${pause_resp}'''),
]
for obj in rows:
  if not obj.get("ok"):
    raise SystemExit(f"voice_control failed: {obj}")
  print(obj.get("rpc_id", ""))
  print(obj.get("tool_call_id", ""))
PY
)

RPC_PLAY="${ids[0]}"
TOOL_PLAY="${ids[1]}"
RPC_SNAPSHOT="${ids[2]}"
TOOL_SNAPSHOT="${ids[3]}"
RPC_PAUSE="${ids[4]}"
TOOL_PAUSE="${ids[5]}"

db_ui_actions_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/ui_actions?session_id=$(python3 -c 'import urllib.parse; print(urllib.parse.quote("""'${SESSION_ID}'"""))')&limit=20&offset=0")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${db_ui_actions_resp}''')
if not obj.get("ok"):
  print("db/ui_actions failed:", obj, file=sys.stderr)
  raise SystemExit(1)
rows = obj.get("ui_actions")
if not isinstance(rows, list) or len(rows) < 3:
  print("expected >=3 ui_actions rows:", rows, file=sys.stderr)
  raise SystemExit(1)
kinds = {((row.get("action") or {}).get("rpc") or {}).get("kind") for row in rows if isinstance(row, dict)}
if not {"media_play", "media_pause", "media_snapshot"}.issubset(kinds):
  print("missing expected voice rpc kinds:", kinds, file=sys.stderr)
  raise SystemExit(1)
for row in rows:
  act = row.get("action") if isinstance(row, dict) else None
  if not isinstance(act, dict):
    continue
  kind = ((act.get("rpc") or {}).get("kind"))
  if kind in {"media_play", "media_pause", "media_snapshot"} and act.get("auto_run") is not True:
    print("expected auto_run=true:", act, file=sys.stderr)
    raise SystemExit(1)
PY

for entry in \
  "${RPC_PLAY}|${TOOL_PLAY}|media_play|true|{\"kind\":\"media_play\",\"ok\":true}" \
  "${RPC_SNAPSHOT}|${TOOL_SNAPSHOT}|media_snapshot|true|{\"kind\":\"media_snapshot\",\"items\":[{\"tag\":\"audio\",\"paused\":false}]}" \
  "${RPC_PAUSE}|${TOOL_PAUSE}|media_pause|true|{\"kind\":\"media_pause\",\"ok\":true}"; do
  IFS='|' read -r rpc_id tool_call_id rpc_kind ok_value result_json <<<"${entry}"
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/json" \
    -d "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${SESSION_ID}",
  "type": "client_rpc_result",
  "client": {"id": "webui", "kind": "webui", "instance_id": "voice-smoke"},
  "data": {
    "rpc_id": "${rpc_id}",
    "request_tool_call_id": "${tool_call_id}",
    "rpc_kind": "${rpc_kind}",
    "ok": "${ok_value}".lower() == "true",
    "result": json.loads(r'''${result_json}''')
  },
  "append_to_session": False
}))
PY
)" \
    "${DAEMON_URL}/api/v1/session/client_event" >/dev/null
done

voice_stats_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/session/voice_stats?session_id=$(python3 -c 'import urllib.parse; print(urllib.parse.quote("""'${SESSION_ID}'"""))')&max_bytes=1048576")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${voice_stats_resp}''')
if not obj.get("ok"):
  print("voice_stats failed:", obj, file=sys.stderr)
  raise SystemExit(1)
counts = obj.get("counts") or {}
expected = {
  "media_play_ok": 1,
  "media_pause_ok": 1,
  "media_snapshot_ok": 1,
}
for key, value in expected.items():
  if counts.get(key) != value:
    print("unexpected voice_stats count", key, counts.get(key), "expected", value, file=sys.stderr)
    raise SystemExit(1)
if obj.get("client_count", 0) < 1:
  print("expected at least one client:", obj.get("clients"), file=sys.stderr)
  raise SystemExit(1)
latest = obj.get("latest_result") or {}
if latest.get("rpc_kind") != "media_pause" or latest.get("ok") is not True:
  print("unexpected latest_result:", latest, file=sys.stderr)
  raise SystemExit(1)
snapshot = obj.get("latest_snapshot") or {}
if (snapshot.get("result") or {}).get("kind") != "media_snapshot":
  print("unexpected latest_snapshot:", snapshot, file=sys.stderr)
  raise SystemExit(1)
recent = obj.get("recent_results")
if not isinstance(recent, list) or len(recent) < 3:
  print("expected >=3 recent_results:", recent, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_session_voice_control_smoke OK"
