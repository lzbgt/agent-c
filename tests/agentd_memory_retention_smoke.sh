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
NAME="agentd_memory_retention_smoke"

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
now = dt.datetime.now()
print(now.strftime('%Y-%m-%d'))
print((now - dt.timedelta(days=1)).strftime('%Y-%m-%d'))
print((now - dt.timedelta(days=10)).strftime('%Y-%m-%d'))
PY
)

today="${dates[0]}"
yesterday="${dates[1]}"
old_day="${dates[2]}"

cat > "${mem_root}/${today}.md" <<'EOF_IN'
# today
EOF_IN
cat > "${mem_root}/${yesterday}.md" <<'EOF_IN'
# yesterday
EOF_IN
cat > "${mem_root}/${old_day}.md" <<'EOF_IN'
# old
EOF_IN

cktimes=()
while IFS= read -r line; do
  cktimes+=("${line}")
done < <(python3 - <<'PY'
import datetime as dt
now = dt.datetime.utcnow()
print(now.strftime('%Y-%m-%dT%H:%M:%SZ'))
print((now - dt.timedelta(days=1)).strftime('%Y-%m-%dT%H:%M:%SZ'))
print((now - dt.timedelta(days=10)).strftime('%Y-%m-%dT%H:%M:%SZ'))
PY
)

ck_now="${cktimes[0]}"
ck_mid="${cktimes[1]}"
ck_old="${cktimes[2]}"

cat > "${mem_root}/checkpoints/structured_${ck_now}.json" <<EOF_IN
{"schema":"agentd_structured_checkpoint_v1","ts_utc":"${ck_now}","path":"STRUCTURED.md","doc":{"items":{"retention.test.key":{"kind":"fact","value":"Old value","status":"active","updated_utc":"${ck_old}","observed_utc":"${ck_old}","sources":["trace:retention"]}}}}
EOF_IN
cat > "${mem_root}/checkpoints/structured_${ck_mid}.json" <<EOF_IN
{"schema":"agentd_structured_checkpoint_v1","ts_utc":"${ck_mid}","path":"STRUCTURED.md","doc":{"items":{}}}
EOF_IN
cat > "${mem_root}/checkpoints/structured_${ck_old}.json" <<EOF_IN
{"schema":"agentd_structured_checkpoint_v1","ts_utc":"${ck_old}","path":"STRUCTURED.md","doc":{"items":{}}}
EOF_IN

resp1="$(curl -fsS --noproxy "*" --max-time 5 \
  -H "Authorization: Bearer ${TOKEN}" \
  -H "Content-Type: application/json" \
  -d '{"dry_run":true,"daily_max_days":1,"checkpoint_max_count":1,"structured_deprecate_days":5,"structured_deprecate_max_entries":10}' \
  "${DAEMON_URL}/api/v1/memory/retention/enforce")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp1}''')
if not obj.get("ok"):
  print(obj, file=sys.stderr)
  raise SystemExit(1)
if int(obj.get("daily_deleted_count", 0)) < 1:
  print("expected daily_deleted_count >= 1", obj, file=sys.stderr)
  raise SystemExit(1)
if int(obj.get("checkpoint_deleted_count", 0)) < 1:
  print("expected checkpoint_deleted_count >= 1", obj, file=sys.stderr)
  raise SystemExit(1)
if int(obj.get("structured_deprecated_count", 0)) < 1:
  print("expected structured_deprecated_count >= 1", obj, file=sys.stderr)
  raise SystemExit(1)
PY

resp2="$(curl -fsS --noproxy "*" --max-time 5 \
  -H "Authorization: Bearer ${TOKEN}" \
  -H "Content-Type: application/json" \
  -d '{"daily_max_days":1,"checkpoint_max_count":1}' \
  "${DAEMON_URL}/api/v1/memory/retention/enforce")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp2}''')
if not obj.get("ok"):
  print(obj, file=sys.stderr)
  raise SystemExit(1)
PY

if [[ ! -f "${mem_root}/${today}.md" ]]; then
  echo "expected ${today}.md to remain" >&2
  exit 1
fi
if [[ -f "${mem_root}/${yesterday}.md" || -f "${mem_root}/${old_day}.md" ]]; then
  echo "expected old daily files to be deleted" >&2
  exit 1
fi

ck_count="$(ls -1 "${mem_root}/checkpoints"/structured_*.json 2>/dev/null | wc -l | tr -d ' ')"
if [[ "${ck_count}" != "1" ]]; then
  echo "expected 1 checkpoint after retention, got ${ck_count}" >&2
  exit 1
fi

echo "agentd_memory_retention_smoke OK"
