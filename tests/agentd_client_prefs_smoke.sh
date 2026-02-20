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
TOKEN="client_prefs_token_123"

trap agentd_smoke_stop EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "agentd_client_prefs_smoke" \
  --tools none \
  --auth-token "${TOKEN}"

agentd_smoke_wait_health "${DAEMON_URL}"

code="$(curl -sS --noproxy "*" --max-time 5 -o /dev/null -w '%{http_code}' \
  "${DAEMON_URL}/api/v1/client/prefs?client_id=webui-test&client_kind=webui")"
if [[ "${code}" != "401" ]]; then
  echo "expected 401 for client prefs without auth, got ${code}" >&2
  exit 1
fi

resp_get="$(curl -sS --noproxy "*" --max-time 5 -H "Authorization: Bearer ${TOKEN}" \
  "${DAEMON_URL}/api/v1/client/prefs?client_id=webui-test&client_kind=webui")"
python3 - <<PY
import json, sys
obj = json.loads(r'''${resp_get}''')
if not obj.get("ok"):
  print("expected ok true, got", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("found"):
  print("expected found false on first fetch", obj, file=sys.stderr)
  raise SystemExit(1)
PY

payload="$(cat <<'JSON'
{"client_id":"webui-test","client_kind":"webui","prefs":{"connection":{"active_profile_id":"p1","profiles":[{"id":"p1","name":"local","mode":"direct","base":"http://127.0.0.1:8123","brokerBase":"https://127.0.0.1:8443","brokerAgentId":"agent1","brokerDeploymentId":""}]}}}
JSON
)"

resp_post="$(curl -sS --noproxy "*" --max-time 5 -H "Authorization: Bearer ${TOKEN}" \
  -H "Content-Type: application/json" -d "${payload}" \
  "${DAEMON_URL}/api/v1/client/prefs")"
python3 - <<PY
import json, sys
obj = json.loads(r'''${resp_post}''')
if not obj.get("ok"):
  print("expected ok true on post", obj, file=sys.stderr)
  raise SystemExit(1)
if not obj.get("found"):
  print("expected found true on post", obj, file=sys.stderr)
  raise SystemExit(1)
prefs = obj.get("prefs") or {}
conn = prefs.get("connection") or {}
if conn.get("active_profile_id") != "p1":
  print("unexpected active_profile_id", conn, file=sys.stderr)
  raise SystemExit(1)
if not obj.get("updated_utc_ms"):
  print("expected updated_utc_ms", obj, file=sys.stderr)
  raise SystemExit(1)
PY

resp_get2="$(curl -sS --noproxy "*" --max-time 5 -H "Authorization: Bearer ${TOKEN}" \
  "${DAEMON_URL}/api/v1/client/prefs?client_id=webui-test&client_kind=webui")"
python3 - <<PY
import json, sys
obj = json.loads(r'''${resp_get2}''')
if not obj.get("ok"):
  print("expected ok true on second get", obj, file=sys.stderr)
  raise SystemExit(1)
if not obj.get("found"):
  print("expected found true on second get", obj, file=sys.stderr)
  raise SystemExit(1)
prefs = obj.get("prefs") or {}
conn = prefs.get("connection") or {}
if conn.get("active_profile_id") != "p1":
  print("unexpected active_profile_id", conn, file=sys.stderr)
  raise SystemExit(1)
profiles = conn.get("profiles") or []
if not profiles:
  print("missing profiles", conn, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_client_prefs_smoke OK"
