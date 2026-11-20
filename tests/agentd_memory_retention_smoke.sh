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
now = dt.datetime.now(dt.timezone.utc)
print(now.strftime('%Y-%m-%dT%H:%M:%SZ'))
print((now - dt.timedelta(days=1)).strftime('%Y-%m-%dT%H:%M:%SZ'))
print((now - dt.timedelta(days=10)).strftime('%Y-%m-%dT%H:%M:%SZ'))
PY
)

ck_now="${cktimes[0]}"
ck_mid="${cktimes[1]}"
ck_old="${cktimes[2]}"

cat > "${mem_root}/checkpoints/structured_${ck_now//:/-}.json" <<EOF_IN
{"schema":"agentd_structured_checkpoint_v1","ts_utc":"${ck_now}","path":"STRUCTURED.md","doc":{"items":{"retention.test.key":{"kind":"fact","value":"Old value","status":"active","updated_utc":"${ck_old}","observed_utc":"${ck_old}","sources":["trace:retention"]}}}}
EOF_IN
cat > "${mem_root}/checkpoints/structured_${ck_mid//:/-}.json" <<EOF_IN
{"schema":"agentd_structured_checkpoint_v1","ts_utc":"${ck_mid}","path":"STRUCTURED.md","doc":{"items":{}}}
EOF_IN
cat > "${mem_root}/checkpoints/structured_${ck_old//:/-}.json" <<EOF_IN
{"schema":"agentd_structured_checkpoint_v1","ts_utc":"${ck_old}","path":"STRUCTURED.md","doc":{"items":{}}}
EOF_IN

# A fresh correction must outrank the older checkpoint. A separate current record
# is old enough to expire, so both selection and successful publication are tested.
MEM_ROOT="${mem_root}" CK_NOW="${ck_now}" CK_OLD="${ck_old}" python3 - <<'PY_FIXTURE'
import json, os, pathlib
items = {
    "retention.test.key": {"kind":"fact", "value":"Corrected current value", "status":"active",
                           "updated_utc":os.environ["CK_NOW"], "observed_utc":os.environ["CK_NOW"]},
    "retention.expired.key": {"kind":"fact", "value":"Expire this current value", "status":"active",
                              "updated_utc":os.environ["CK_OLD"], "observed_utc":os.environ["CK_OLD"]},
}
text = "<!-- AGENT_MEMORY_V1_BEGIN -->\n" + json.dumps({"schema":"agent_memory_v2", "items":items}) + "\n<!-- AGENT_MEMORY_V1_END -->\n"
(pathlib.Path(os.environ["MEM_ROOT"]) / "STRUCTURED.md").write_text(text)
PY_FIXTURE

resp1="$(curl -sS --noproxy "*" --max-time 5 \
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
if int(obj.get("daily_selected_count", 0)) < 1:
  print("expected daily_deleted_count >= 1", obj, file=sys.stderr)
  raise SystemExit(1)
if int(obj.get("checkpoint_selected_count", 0)) < 1:
  print("expected checkpoint_deleted_count >= 1", obj, file=sys.stderr)
  raise SystemExit(1)
if int(obj.get("structured_selected_count", 0)) != 1:
  print("expected exactly the one expired current record", obj, file=sys.stderr)
  raise SystemExit(1)
PY

# Capture fixture bytes before retirement, so receipts must prove retained originals.
EXPECTED="${LOG_DIR}/${NAME}_${PORT}.originals.json" MEM_ROOT="${mem_root}" python3 - <<'PY_ORIGINALS'
import json, os, pathlib
root = pathlib.Path(os.environ["MEM_ROOT"])
files = list(root.glob("*.md")) + list((root / "checkpoints").glob("structured_*.json"))
pathlib.Path(os.environ["EXPECTED"]).write_text(json.dumps({str(p.relative_to(root)): p.read_bytes().hex() for p in files}))
PY_ORIGINALS

resp2="$(curl -sS --noproxy "*" --max-time 5 \
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

EXPECTED="${LOG_DIR}/${NAME}_${PORT}.originals.json" MEM_ROOT="${mem_root}" RESPONSE="${resp2}" python3 - <<'PY_RECEIPTS'
import hashlib, json, os, pathlib
root = pathlib.Path(os.environ["MEM_ROOT"])
response = json.loads(os.environ["RESPONSE"])
assert response["schema"] == "agent.memory.retention.v2" and response["complete"]
assert response["daily_deleted_count"] == 2 and response["checkpoint_deleted_count"] == 2
originals = json.loads(pathlib.Path(os.environ["EXPECTED"]).read_text())
for receipt in response["outcomes"]:
    assert receipt["commit_state"] == "retired" and receipt["current_revision"] == "absent"
    raw = bytes.fromhex(originals[receipt["path"]])
    digest = hashlib.sha256(raw).hexdigest()
    assert receipt["expected_revision"] == "sha256:" + digest
    reference = receipt["original_evidence"]
    assert reference["revision"] == "sha256:" + digest
    assert reference["span"] == {"kind":"bytes", "begin":0, "end":len(raw)}
    archived = root / ".revisions" / hashlib.sha256(receipt["path"].encode()).hexdigest() / (digest + ".blob")
    assert archived.read_bytes() == raw and not (root / receipt["path"]).exists()
assert response["daily_bytes_after"] == response["daily_bytes_projected"]
PY_RECEIPTS

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

if [[ ! -f "${mem_root}/checkpoints/structured_${ck_now//:/-}.json" ]]; then
  echo "expected newest checkpoint to remain" >&2
  exit 1
fi

resp3="$(curl -sS --noproxy "*" --max-time 5 \
  -H "Authorization: Bearer ${TOKEN}" -H "Content-Type: application/json" \
  -d '{"structured_deprecate_days":5,"structured_deprecate_max_entries":10}' \
  "${DAEMON_URL}/api/v1/memory/retention/enforce")"
MEM_ROOT="${mem_root}" RESPONSE="${resp3}" python3 - <<'PY_VERIFY'
import json, os, pathlib
receipt = json.loads(os.environ["RESPONSE"])
assert receipt["ok"] and receipt["structured_deprecated_count"] == 1, receipt
text = (pathlib.Path(os.environ["MEM_ROOT"]) / "STRUCTURED.md").read_text()
body = text.split("<!-- AGENT_MEMORY_V1_BEGIN -->", 1)[1].split("<!-- AGENT_MEMORY_V1_END -->", 1)[0]
items = json.loads(body)["items"]
assert items["retention.test.key"]["value"] == "Corrected current value", items
assert items["retention.test.key"]["status"] == "active", items
assert items["retention.expired.key"]["status"] == "deprecated", items
PY_VERIFY

# A storage failure preserves that source while an independent source retires.
MEM_ROOT="${mem_root}" python3 - <<'PY_PARTIAL_FIXTURE'
import hashlib, os, pathlib
root = pathlib.Path(os.environ["MEM_ROOT"])
(root / "2001-01-01.md").write_text("preserve original on failure\n")
(root / "2001-01-02.md").write_text("independent source\n")
(root / ".revisions" / hashlib.sha256(b"2001-01-01.md").hexdigest()).write_text("blocked archive directory")
PY_PARTIAL_FIXTURE
partial_file="${LOG_DIR}/${NAME}_${PORT}.partial.json"
status="$(curl -sS --noproxy "*" --max-time 5 -o "${partial_file}" -w '%{http_code}' \
  -H "Authorization: Bearer ${TOKEN}" -H "Content-Type: application/json" \
  -d '{"daily_max_days":1}' "${DAEMON_URL}/api/v1/memory/retention/enforce")"
if [[ "${status}" != 409 ]]; then
  echo "expected HTTP 409 for incomplete retention, got ${status}" >&2
  exit 1
fi
PARTIAL="${partial_file}" MEM_ROOT="${mem_root}" python3 - <<'PY_PARTIAL_VERIFY'
import json, os, pathlib
response = json.loads(pathlib.Path(os.environ["PARTIAL"]).read_text())
assert response["ok"] is False and response["complete"] is False
assert response["daily_deleted_count"] == 1 and response["daily_selected_count"] == 2
assert [r["commit_state"] for r in response["outcomes"]] == ["not_retired", "retired"]
root = pathlib.Path(os.environ["MEM_ROOT"])
assert (root / "2001-01-01.md").read_text() == "preserve original on failure\n"
assert not (root / "2001-01-02.md").exists()
PY_PARTIAL_VERIFY

echo "agentd_memory_retention_smoke OK"
