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
NAME="agentd_memory_consolidate_smoke"

trap agentd_smoke_stop EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT}" "${NAME}" \
  --tools host \
  --auth-token "${TOKEN}"

agentd_smoke_wait_health "${DAEMON_URL}"

LOG_DIR="$(agentd_smoke_log_dir)"
STATE_DIR="${LOG_DIR}/${NAME}_${PORT}.state"

today="$(date +%F)"
mem_root="${STATE_DIR}/memory"
mkdir -p "${mem_root}"

cat > "${mem_root}/${today}.md" <<'EOF'
### note
- @mem fact ui.rendering = Scene rendering must survive refresh + restart
- @mem deprecated feature.a = Feature set A
EOF

resp1="$(curl -fsS --noproxy "*" --max-time 5 \
  -H "Authorization: Bearer ${TOKEN}" \
  -H "Content-Type: application/json" \
  -d '{"daily_days":1,"max_entries":64}' \
  "${DAEMON_URL}/api/v1/memory/consolidate")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp1}''')
if not obj.get("ok"):
  print(obj, file=sys.stderr)
  raise SystemExit(1)
data = obj.get("data") or {}
if int(data.get("marker_lines_found", 0)) < 1:
  print("expected marker_lines_found>=1", data, file=sys.stderr)
  raise SystemExit(1)
mpr = data.get("memory_put_response") or {}
if not mpr.get("ok"):
  print("expected memory_put_response.ok true", mpr, file=sys.stderr)
  raise SystemExit(1)
mpd = mpr.get("data") or {}
if not mpd.get("structured"):
  print("expected structured write", mpd, file=sys.stderr)
  raise SystemExit(1)
PY

if [[ ! -f "${mem_root}/STRUCTURED.md" ]]; then
  echo "expected STRUCTURED.md to exist at ${mem_root}/STRUCTURED.md" >&2
  exit 1
fi
if ! rg -n "ui\\.rendering" "${mem_root}/STRUCTURED.md" >/dev/null 2>&1; then
  echo "expected ui.rendering in STRUCTURED.md" >&2
  exit 1
fi

ck_count_1="$(ls -1 "${mem_root}/checkpoints"/structured_*.json 2>/dev/null | wc -l | tr -d ' ')"
if [[ "${ck_count_1}" -lt 1 ]]; then
  echo "expected at least 1 checkpoint, got ${ck_count_1}" >&2
  exit 1
fi

# Second consolidation should be idempotent (no changes, no new checkpoint).
resp2="$(curl -fsS --noproxy "*" --max-time 5 \
  -H "Authorization: Bearer ${TOKEN}" \
  -H "Content-Type: application/json" \
  -d '{"daily_days":1,"max_entries":64}' \
  "${DAEMON_URL}/api/v1/memory/consolidate")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${resp2}''')
if not obj.get("ok"):
  print(obj, file=sys.stderr)
  raise SystemExit(1)
data = obj.get("data") or {}
mpr = data.get("memory_put_response") or {}
if not mpr.get("ok"):
  print("expected memory_put_response.ok true", mpr, file=sys.stderr)
  raise SystemExit(1)
mpd = mpr.get("data") or {}
if not mpd.get("no_changes"):
  print("expected no_changes on second consolidation", mpd, file=sys.stderr)
  raise SystemExit(1)
PY

ck_count_2="$(ls -1 "${mem_root}/checkpoints"/structured_*.json 2>/dev/null | wc -l | tr -d ' ')"
if [[ "${ck_count_2}" != "${ck_count_1}" ]]; then
  echo "expected checkpoint count stable (idempotent), was ${ck_count_1} now ${ck_count_2}" >&2
  exit 1
fi

echo "agentd_memory_consolidate_smoke OK"

