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
TOKEN="test_token_123"
NAME="agentd_memory_recaps_smoke"

trap agentd_smoke_stop EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "${NAME}" \
  --auth-token "${TOKEN}"

agentd_smoke_wait_health "${DAEMON_URL}"

LOG_DIR="$(agentd_smoke_log_dir)"
STATE_DIR="${LOG_DIR}/${NAME}_${PORT}.state"
mem_root="${STATE_DIR}/memory"
mkdir -p "${mem_root}/checkpoints"

readarray -t dates < <(python3 - <<'PY'
import datetime as dt
now = dt.datetime.utcnow()
print(now.strftime('%Y-%m-%d'))
print(now.strftime('%Y-%m-%dT%H:%M:%SZ'))
PY
)

today="${dates[0]}"
now_utc="${dates[1]}"

echo "- @obs Recap should summarize recent work." > "${mem_root}/${today}.md"
cat >> "${mem_root}/${today}.md" <<EOF_IN
  - ts_utc: ${now_utc}
  - importance: 3
EOF_IN

cat > "${mem_root}/checkpoints/structured_${now_utc}.json" <<EOF_IN
{"schema":"agentd_structured_checkpoint_v1","ts_utc":"${now_utc}","path":"STRUCTURED.md","doc":{"items":{"recap.test.key":{"kind":"fact","value":"Recap summarization should be deterministic","status":"active","updated_utc":"${now_utc}","observed_utc":"${now_utc}","sources":["trace:recap"]}}}}
EOF_IN

resp="$(curl -fsS --noproxy "*" --max-time 5 \
  -H "Authorization: Bearer ${TOKEN}" \
  -H "Content-Type: application/json" \
  -X POST \
  -d '{"dry_run":true,"daily_days":1,"max_items":5}' \
  "${DAEMON_URL}/api/v1/memory/recaps")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp}''')
if not obj.get("ok"):
  print(obj, file=sys.stderr)
  raise SystemExit(1)
if not obj.get("dry_run"):
  print("expected dry_run", obj, file=sys.stderr)
  raise SystemExit(1)
structured = obj.get("structured_items") or []
daily = obj.get("daily_items") or []
if not structured:
  print("expected structured_items", obj, file=sys.stderr)
  raise SystemExit(1)
if not daily:
  print("expected daily_items", obj, file=sys.stderr)
  raise SystemExit(1)
prompt = obj.get("prompt") or ""
if not prompt:
  print("expected prompt", obj, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_memory_recaps_smoke OK"
