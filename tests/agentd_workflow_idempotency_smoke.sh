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

DB_PATH="${LOG_DIR}/agentd_workflow_idempotency_smoke_${PORT_DAEMON}.sqlite"
STATE_DIR="${LOG_DIR}/agentd_workflow_idempotency_smoke_${PORT_DAEMON}.state"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_workflow_idempotency_smoke" \
  --db-path "${DB_PATH}" \
  --state-dir "${STATE_DIR}" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

IDEM_KEY="idem_key_001"

submit1="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<'PY'
import json

payload = {
  "workflow_id": "wf_idem_A",
  "trace_id": "trace_idem_A",
  "idempotency_key": "idem_key_001",
  "tasks": [
    {"task_id": "A", "kind": "delay", "delay_ms": 0, "result": {"assistant_text": "A"}}
  ],
}
print(json.dumps(payload))
PY
)" \
  "${DAEMON_URL}/api/v1/workflow/submit")"

wf1="$(python3 - <<PY
import json
obj = json.loads(r'''${submit1}''')
print(obj.get("workflow_id",""))
PY
)"
dedup1="$(python3 - <<PY
import json
obj = json.loads(r'''${submit1}''')
print("1" if obj.get("deduped") else "0")
PY
)"
if [[ "${wf1}" != "wf_idem_A" ]]; then
  echo "expected workflow_id=wf_idem_A, got: ${submit1}" >&2
  exit 1
fi
if [[ "${dedup1}" != "0" ]]; then
  echo "expected first submit deduped=false, got: ${submit1}" >&2
  exit 1
fi

submit2="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<'PY'
import json

payload = {
  "workflow_id": "wf_idem_B",
  "trace_id": "trace_idem_B",
  "idempotency_key": "idem_key_001",
  "tasks": [
    {"task_id": "A", "kind": "delay", "delay_ms": 0, "result": {"assistant_text": "A"}}
  ],
}
print(json.dumps(payload))
PY
)" \
  "${DAEMON_URL}/api/v1/workflow/submit")"

wf2="$(python3 - <<PY
import json
obj = json.loads(r'''${submit2}''')
print(obj.get("workflow_id",""))
PY
)"
dedup2="$(python3 - <<PY
import json
obj = json.loads(r'''${submit2}''')
print("1" if obj.get("deduped") else "0")
PY
)"
if [[ "${wf2}" != "wf_idem_A" ]]; then
  echo "expected second submit to return original workflow_id, got: ${submit2}" >&2
  exit 1
fi
if [[ "${dedup2}" != "1" ]]; then
  echo "expected second submit deduped=true, got: ${submit2}" >&2
  exit 1
fi

python3 - <<PY
import sqlite3, sys

db_path = r"""${DB_PATH}"""
con = sqlite3.connect(db_path)
cur = con.cursor()
cur.execute("SELECT COUNT(1) FROM workflows WHERE idempotency_key=?", ("${IDEM_KEY}",))
cnt = int(cur.fetchone()[0])
if cnt != 1:
  print("expected exactly 1 workflow row for idempotency_key", cnt, file=sys.stderr)
  raise SystemExit(1)
cur.execute("SELECT workflow_id, trace_id FROM workflows WHERE idempotency_key=? LIMIT 1", ("${IDEM_KEY}",))
row = cur.fetchone()
if not row or row[0] != "wf_idem_A":
  print("expected stored workflow_id wf_idem_A, got", row, file=sys.stderr)
  raise SystemExit(1)
if row[1] != "trace_idem_A":
  print("expected stored trace_id trace_idem_A, got", row, file=sys.stderr)
  raise SystemExit(1)
PY

snap="$(curl -fsS --noproxy "*" --max-time 20 \
  "${DAEMON_URL}/api/v1/workflow?workflow_id=${wf1}&include_tasks=0&include_results=0")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${snap}''')
wf = (obj.get("workflow") or {})
if wf.get("idempotency_key") != "${IDEM_KEY}":
  print("expected workflow.idempotency_key to roundtrip", wf, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_workflow_idempotency_smoke OK"

