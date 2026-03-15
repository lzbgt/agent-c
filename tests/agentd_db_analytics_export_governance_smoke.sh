#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "usage: agentd_db_analytics_export_governance_smoke.sh <agentd_bin>" >&2
  exit 2
fi

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

PORT_DAEMON="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"
NAME="agentd_db_analytics_export_governance_smoke"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}"
agentd_smoke_wait_health "${DAEMON_URL}"

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "workflow_id": "${NAME}_workflow",
  "tasks": [
    {
      "task_id": "A",
      "kind": "delay",
      "delay_ms": 5,
      "result": {"assistant_text": "done"}
    }
  ]
}))
PY
)" \
  "${DAEMON_URL}/api/v1/workflow/submit")"

workflow_id="$(python3 - <<PY
import json, sys
obj = json.loads(r'''${submit_resp}''')
workflow_id = obj.get("workflow_id", "")
if not workflow_id:
  print("missing workflow_id", obj, file=sys.stderr)
  raise SystemExit(1)
print(workflow_id)
PY
)"

final=""
for _ in $(seq 1 200); do
  final="$(curl -fsS --noproxy "*" --max-time 10 \
    "${DAEMON_URL}/api/v1/workflow?workflow_id=${workflow_id}&include_tasks=1&include_results=1")"
  if python3 - <<PY >/dev/null 2>&1
import json
obj = json.loads(r'''${final}''')
w = obj.get("workflow") or {}
raise SystemExit(0 if w.get("status") in ("done", "error", "cancelled") else 1)
PY
  then
    break
  fi
  sleep 0.05
done

python3 - <<PY
import json, sys
obj = json.loads(r'''${final}''')
if not obj.get("ok"):
  print("workflow get failed", obj, file=sys.stderr)
  raise SystemExit(1)
w = obj.get("workflow") or {}
if w.get("status") != "done":
  print("expected workflow done", w, file=sys.stderr)
  raise SystemExit(1)
PY

workflow_export_json="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/analytics/workflows/export?format=json&scope=all")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${workflow_export_json}''')
if not obj.get("ok"):
  print("workflow export json failed", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("scope") != "all":
  print("unexpected workflow export scope", obj, file=sys.stderr)
  raise SystemExit(1)
if not isinstance(obj.get("generated_utc_ms"), int):
  print("missing generated_utc_ms", obj, file=sys.stderr)
  raise SystemExit(1)
if "durable" not in obj or "edge" not in obj:
  print("expected durable and edge sections", obj, file=sys.stderr)
  raise SystemExit(1)
PY

workflow_csv_headers="${LOG_DIR}/${NAME}.workflow_export.headers"
workflow_csv_body="${LOG_DIR}/${NAME}.workflow_export.csv"
workflow_csv_status="$(curl -sS --noproxy "*" --max-time 10 \
  -D "${workflow_csv_headers}" -o "${workflow_csv_body}" -w "%{http_code}" \
  "${DAEMON_URL}/api/v1/db/analytics/workflows/export?format=csv&scope=all")"

python3 - <<PY
import pathlib, sys
status = int(r'''${workflow_csv_status}''')
headers = pathlib.Path(r'''${workflow_csv_headers}''').read_text(encoding='utf-8')
body = pathlib.Path(r'''${workflow_csv_body}''').read_text(encoding='utf-8')
if status != 200:
  print("workflow export csv status", status, file=sys.stderr)
  raise SystemExit(1)
if 'Content-Disposition: attachment; filename="workflow_analytics_export.csv"' not in headers:
  print("missing workflow export attachment header", headers, file=sys.stderr)
  raise SystemExit(1)
if "section,metric,key,value" not in body:
  print("missing workflow export csv header", body, file=sys.stderr)
  raise SystemExit(1)
if "durable" not in body or "edge" not in body:
  print("expected durable and edge rows in workflow export csv", body, file=sys.stderr)
  raise SystemExit(1)
PY

edge_export_json="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/db/analytics/edge/export?format=json&scope=all&active_within_ms=600000")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${edge_export_json}''')
if not obj.get("ok"):
  print("edge export json failed", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("scope") != "all":
  print("unexpected edge export scope", obj, file=sys.stderr)
  raise SystemExit(1)
if not isinstance(obj.get("generated_utc_ms"), int):
  print("missing edge generated_utc_ms", obj, file=sys.stderr)
  raise SystemExit(1)
if "edge_tasks" not in obj or "edge_nodes" not in obj:
  print("expected edge_tasks and edge_nodes", obj, file=sys.stderr)
  raise SystemExit(1)
PY

edge_csv_headers="${LOG_DIR}/${NAME}.edge_export.headers"
edge_csv_body="${LOG_DIR}/${NAME}.edge_export.csv"
edge_csv_status="$(curl -sS --noproxy "*" --max-time 10 \
  -D "${edge_csv_headers}" -o "${edge_csv_body}" -w "%{http_code}" \
  "${DAEMON_URL}/api/v1/db/analytics/edge/export?format=csv&scope=all&active_within_ms=600000")"

python3 - <<PY
import pathlib, sys
status = int(r'''${edge_csv_status}''')
headers = pathlib.Path(r'''${edge_csv_headers}''').read_text(encoding='utf-8')
body = pathlib.Path(r'''${edge_csv_body}''').read_text(encoding='utf-8')
if status != 200:
  print("edge export csv status", status, file=sys.stderr)
  raise SystemExit(1)
if 'Content-Disposition: attachment; filename="edge_analytics_export.csv"' not in headers:
  print("missing edge export attachment header", headers, file=sys.stderr)
  raise SystemExit(1)
if "section,metric,key,value" not in body:
  print("missing edge export csv header", body, file=sys.stderr)
  raise SystemExit(1)
if "edge_tasks" not in body or "edge_nodes" not in body:
  print("expected edge_tasks and edge_nodes rows in edge export csv", body, file=sys.stderr)
  raise SystemExit(1)
PY

echo "ok: ${NAME}"
