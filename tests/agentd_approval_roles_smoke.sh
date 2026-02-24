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
NAME="agentd_approval_roles_smoke"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}" \
  --tools host \
  --yolo

agentd_smoke_wait_health "${DAEMON_URL}"

DB_PATH="${LOG_DIR}/${NAME}_${PORT_DAEMON}.sqlite"
APPROVAL_ID="approval_roles_${PORT_DAEMON}"

python3 - <<PY
import json
import sqlite3
import time

path = "${DB_PATH}"
approval_id = "${APPROVAL_ID}"
now = int(time.time() * 1000)
roles = ["security", "reviewer"]

conn = sqlite3.connect(path)
cur = conn.cursor()
cur.execute(
  """
  INSERT INTO approval_requests(
    approval_id, tool_name, required_approvals, role_constraints_json, status, created_unix_ms
  ) VALUES(?,?,?,?,?,?);
  """,
  (approval_id, "shell_exec", 1, json.dumps(roles), "pending", now),
)
conn.commit()
conn.close()
PY

payload_missing_role="$(python3 - <<PY
import json
print(json.dumps({"member_id": "alice", "decision": "approve"}))
PY
)"
code_missing_role="$(curl -sS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${payload_missing_role}" \
  -o "${LOG_DIR}/${NAME}_missing_role.json" \
  -w "%{http_code}" \
  "${DAEMON_URL}/api/v1/approvals/${APPROVAL_ID}/decisions")"
if [[ "${code_missing_role}" != "400" ]]; then
  echo "expected 400 for missing member_role, got ${code_missing_role}" >&2
  cat "${LOG_DIR}/${NAME}_missing_role.json" >&2 || true
  exit 1
fi

payload_wrong_role="$(python3 - <<PY
import json
print(json.dumps({"member_id": "alice", "member_role": "ops", "decision": "approve"}))
PY
)"
code_wrong_role="$(curl -sS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${payload_wrong_role}" \
  -o "${LOG_DIR}/${NAME}_wrong_role.json" \
  -w "%{http_code}" \
  "${DAEMON_URL}/api/v1/approvals/${APPROVAL_ID}/decisions")"
if [[ "${code_wrong_role}" != "403" ]]; then
  echo "expected 403 for disallowed member_role, got ${code_wrong_role}" >&2
  cat "${LOG_DIR}/${NAME}_wrong_role.json" >&2 || true
  exit 1
fi

payload_ok="$(python3 - <<PY
import json
print(json.dumps({"member_id": "alice", "member_role": "security", "decision": "approve", "note": "ok"}))
PY
)"
resp_ok="$(curl -sS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${payload_ok}" \
  "${DAEMON_URL}/api/v1/approvals/${APPROVAL_ID}/decisions")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp_ok}''')
if not obj.get("ok"):
  print("approval decision not ok:", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("status") != "approved":
  print("expected approved status:", obj, file=sys.stderr)
  raise SystemExit(1)
decision = obj.get("decision") or {}
if decision.get("member_role") != "security":
  print("expected member_role security:", decision, file=sys.stderr)
  raise SystemExit(1)
PY

detail="$(curl -sS --noproxy "*" --max-time 10 "${DAEMON_URL}/api/v1/approvals/${APPROVAL_ID}")"
python3 - <<PY
import json, sys
obj = json.loads(r'''${detail}''')
if not obj.get("ok"):
  print("approval detail not ok:", obj, file=sys.stderr)
  raise SystemExit(1)
approval = obj.get("approval") or {}
if approval.get("status") != "approved":
  print("approval status not approved:", approval, file=sys.stderr)
  raise SystemExit(1)
decisions = obj.get("decisions") or []
if not decisions:
  print("expected decisions array", obj, file=sys.stderr)
  raise SystemExit(1)
if decisions[-1].get("member_role") != "security":
  print("expected member_role in decisions", decisions[-1], file=sys.stderr)
  raise SystemExit(1)
PY

echo "${NAME} OK"
