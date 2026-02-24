#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

ROOT="$(agentd_smoke_project_root)"
LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

STATE_PATH="${BROKER_SMOKE_STATE:-${ROOT}/out/devstack_state.json}"
BROKER_BASE="${BROKER_BASE:-}"
KEYCLOAK_BASE="${KEYCLOAK_BASE:-}"

if [[ -z "${BROKER_BASE}" || -z "${KEYCLOAK_BASE}" ]]; then
  if [[ -f "${STATE_PATH}" ]]; then
    BROKER_BASE="$(python3 - <<PY
import json
with open("${STATE_PATH}") as f:
    st=json.load(f)
print(st.get("broker_base",""))
PY
)"
    KEYCLOAK_BASE="$(python3 - <<PY
import json
with open("${STATE_PATH}") as f:
    st=json.load(f)
print(st.get("keycloak_base",""))
PY
)"
  fi
fi

if [[ -z "${BROKER_BASE}" || -z "${KEYCLOAK_BASE}" ]]; then
  echo "broker_client_prefs_smoke: missing BROKER_BASE/KEYCLOAK_BASE or devstack_state.json" >&2
  exit 77
fi

CLIENT_ID="webui-smoke-$(python3 - <<'PY'
import secrets
print(secrets.token_hex(6))
PY
)"

TOKEN="$(agentd_smoke_curl -fsS -k \
  -d 'grant_type=password' \
  -d 'client_id=agentd-broker-dev' \
  -d 'username=test' \
  -d 'password=test' \
  "${KEYCLOAK_BASE}/realms/agentd/protocol/openid-connect/token" \
  | python3 -c 'import json,sys; print(json.load(sys.stdin).get("access_token",""))')"

if [[ -z "${TOKEN}" ]]; then
  echo "broker_client_prefs_smoke: missing OIDC token" >&2
  exit 2
fi

resp_get="$(agentd_smoke_curl -sS -k -H "Authorization: Bearer ${TOKEN}" \
  "${BROKER_BASE}/v1/client_prefs?client_id=${CLIENT_ID}&client_kind=webui")"
python3 - <<PY
import json, sys
obj = json.loads(r'''${resp_get}''')
if not obj.get("ok"):
  print("expected ok true on first get", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("found"):
  print("expected found false on first get", obj, file=sys.stderr)
  raise SystemExit(1)
PY

payload="$(cat <<JSON
{"client_id":"${CLIENT_ID}","client_kind":"webui","prefs":{"connection":{"active_profile_id":"p1","profiles":[{"id":"p1","name":"smoke","mode":"broker","base":"","brokerBase":"${BROKER_BASE}","brokerAgentId":"agent1","brokerDeploymentId":""}]}}}
JSON
)"

resp_post="$(agentd_smoke_curl -sS -k -H "Authorization: Bearer ${TOKEN}" \
  -H "Content-Type: application/json" -d "${payload}" \
  "${BROKER_BASE}/v1/client_prefs")"
python3 - <<PY
import json, sys
obj = json.loads(r'''${resp_post}''')
if not obj.get("ok"):
  print("expected ok true on post", obj, file=sys.stderr)
  raise SystemExit(1)
if not obj.get("found"):
  print("expected found true on post", obj, file=sys.stderr)
  raise SystemExit(1)
if not obj.get("updated_utc_ms"):
  print("expected updated_utc_ms", obj, file=sys.stderr)
  raise SystemExit(1)
prefs = obj.get("prefs") or {}
conn = prefs.get("connection") or {}
if conn.get("active_profile_id") != "p1":
  print("unexpected active_profile_id", conn, file=sys.stderr)
  raise SystemExit(1)
PY

resp_get2="$(agentd_smoke_curl -sS -k -H "Authorization: Bearer ${TOKEN}" \
  "${BROKER_BASE}/v1/client_prefs?client_id=${CLIENT_ID}&client_kind=webui")"
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

echo "broker_client_prefs_smoke OK"
