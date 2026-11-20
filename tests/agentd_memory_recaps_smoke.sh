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
mkdir -p "${mem_root}/recaps"

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

echo "- @obs Recap should summarize recent work." > "${mem_root}/${today}.md"
cat >> "${mem_root}/${today}.md" <<EOF_IN
  - ts_utc: ${now_utc}
  - importance: 3
EOF_IN

cat > "${mem_root}/checkpoints/structured_${now_utc}.json" <<EOF_IN
{"schema":"agentd_structured_checkpoint_v1","ts_utc":"${now_utc}","path":"STRUCTURED.md","doc":{"items":{"recap.test.key":{"kind":"fact","value":"Recap summarization should be deterministic","status":"active","updated_utc":"${now_utc}","observed_utc":"${now_utc}","sources":["trace:recap"]}}}}
EOF_IN

cat > "${mem_root}/recaps/recap_daily_fixture.json" <<EOF_IN
{"schema":"agentd_memory_recap_v1","ts_utc":"${now_utc}","ts_utc_ms":0,"model":"stub","kind":"daily","summary_text":"Daily recap stub","structured_items":[],"daily_items":[]}
EOF_IN

MEM_ROOT="${mem_root}" python3 - <<'PY_CURRENT'
import json, os, pathlib
root = pathlib.Path(os.environ["MEM_ROOT"])
checkpoint = next((root / "checkpoints").glob("structured_*.json"))
doc = json.loads(checkpoint.read_text())["doc"]
doc["schema"] = "agent_memory_v2"
(root / "STRUCTURED.md").write_text("<!-- AGENT_MEMORY_V1_BEGIN -->\n" + json.dumps(doc) + "\n<!-- AGENT_MEMORY_V1_END -->\n")
PY_CURRENT

resp="$(curl -fsS --noproxy "*" --max-time 5 \
  -H "Authorization: Bearer ${TOKEN}" \
  -H "Content-Type: application/json" \
  -X POST \
  -d '{"dry_run":true,"daily_days":1,"max_items":5,"kind":"daily"}' \
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
if obj.get("kind") != "daily":
  print("expected kind", obj, file=sys.stderr)
  raise SystemExit(1)
structured = obj.get("structured_items") or []
daily = obj.get("daily_items") or []
if not structured:
  print("expected structured_items", obj, file=sys.stderr)
  raise SystemExit(1)
if not daily:
  print("expected daily_items", obj, file=sys.stderr)
  raise SystemExit(1)
sources = (obj.get("derivation") or {}).get("evidence") or []
if not sources:
  print("expected immutable evidence", obj, file=sys.stderr)
  raise SystemExit(1)
prompt = obj.get("prompt") or ""
if not prompt:
  print("expected prompt", obj, file=sys.stderr)
  raise SystemExit(1)
PY

list_resp="$(curl -fsS --noproxy "*" --max-time 5 \
  -H "Authorization: Bearer ${TOKEN}" \
  "${DAEMON_URL}/api/v1/memory/recaps?view=historical&limit=5&kind=daily")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${list_resp}''')
if not obj.get("ok"):
  print(obj, file=sys.stderr)
  raise SystemExit(1)
recaps = obj.get("recaps") or []
if not recaps:
  print("expected recap list", obj, file=sys.stderr)
  raise SystemExit(1)
if any(r.get("kind") != "daily" for r in recaps):
  print("unexpected recap kind filter", obj, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_memory_recaps_smoke OK"
