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
NAME="agentd_memory_salience_smoke"

trap agentd_smoke_stop EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "${NAME}" \
  --auth-token "${TOKEN}"

agentd_smoke_wait_health "${DAEMON_URL}"

LOG_DIR="$(agentd_smoke_log_dir)"
STATE_DIR="${LOG_DIR}/${NAME}_${PORT}.state"
mem_root="${STATE_DIR}/memory"
mkdir -p "${mem_root}/checkpoints"

dates=()
while IFS= read -r line; do
  dates+=("${line}")
done < <(python3 - <<'PY'
import datetime as dt
now = dt.datetime.utcnow()
print(now.strftime('%Y-%m-%d'))
print(now.strftime('%Y-%m-%dT%H:%M:%SZ'))
PY
)

today="${dates[0]}"
now_utc="${dates[1]}"

echo "- @obs Salience should prefer recent, important facts." > "${mem_root}/${today}.md"
cat >> "${mem_root}/${today}.md" <<EOF_IN
  - ts_utc: ${now_utc}
  - importance: 4
EOF_IN

cat > "${mem_root}/checkpoints/structured_${now_utc}.json" <<EOF_IN
{"schema":"agentd_structured_checkpoint_v1","ts_utc":"${now_utc}","path":"STRUCTURED.md","doc":{"items":{"salience.test.key":{"kind":"fact","value":"Salience must be deterministic","status":"active","updated_utc":"${now_utc}","observed_utc":"${now_utc}","sources":["trace:salience"]}}}}
EOF_IN

resp="$(curl -fsS --noproxy "*" --max-time 5 \
  -H "Authorization: Bearer ${TOKEN}" \
  "${DAEMON_URL}/api/v1/memory/salience?daily_days=1&max_items=5")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp}''')
if not obj.get("ok"):
  print(obj, file=sys.stderr)
  raise SystemExit(1)
structured = obj.get("structured_items") or []
daily = obj.get("daily_items") or []
if not structured:
  print("expected structured_items", obj, file=sys.stderr)
  raise SystemExit(1)
if not daily:
  print("expected daily_items", obj, file=sys.stderr)
  raise SystemExit(1)
if structured[0].get("key") != "salience.test.key":
  print("unexpected structured key", structured[0], file=sys.stderr)
  raise SystemExit(1)
if "${today}.md" not in str(daily[0].get("path")):
  print("unexpected daily path", daily[0], file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_memory_salience_smoke OK"
