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
NAME="agentd_workflow_wrr_session_weight_smoke"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

# Deterministic scheduler test:
# - workflow engine concurrency=1 (serial execution)
# - session A has weight=2
# - session B has weight=1
# Expected completion order follows a weighted round-robin cycle A,A,B (up to rotation),
# so in the first 5 completions we should observe a sequence consistent with repeating "AAB".
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}" \
  --tools none \
  --workflow-concurrency 1 \
  --workflow-poll-ms 1000 \
  --workflow-max-inflight-per-workflow 1 \
  --workflow-fair-queue-policy wrr

agentd_smoke_wait_health "${DAEMON_URL}"

submit_one() {
  local session_id="${1}"
  local session_weight="${2}"
  local tag="${3}"
  local delay_ms="${4}"
  curl -fsS --noproxy "*" --max-time 20 \
    -H "Content-Type: application/json" \
    -d "$(python3 - <<PY
import json
print(json.dumps({
  "allow_sessions": True,
  "session_id": "${session_id}",
  "session_weight": int(${session_weight}),
  "tasks": [
    {"task_id": "T", "kind": "delay", "delay_ms": int(${delay_ms}), "result": {"assistant_text": "${tag}"}}
  ]
}))
PY
)" \
    "${DAEMON_URL}/api/v1/workflow/submit"
}

WIDS=()
for tag in A1 A2 A3; do
  resp="$(submit_one "sess_A" 2 "${tag}" 30)"
  wid="$(python3 - <<PY
import json
print(json.loads(r'''${resp}''').get("workflow_id",""))
PY
)"
  if [[ -z "${wid}" ]]; then
    echo "failed to get workflow_id for ${tag}: ${resp}" >&2
    exit 1
  fi
  WIDS+=("${wid}")
done
for tag in B1 B2; do
  resp="$(submit_one "sess_B" 1 "${tag}" 30)"
  wid="$(python3 - <<PY
import json
print(json.loads(r'''${resp}''').get("workflow_id",""))
PY
)"
  if [[ -z "${wid}" ]]; then
    echo "failed to get workflow_id for ${tag}: ${resp}" >&2
    exit 1
  fi
  WIDS+=("${wid}")
done

# Wait until all 5 workflows are done.
stats=""
done_cnt=0
for _ in $(seq 1 500); do
  stats="$(curl -fsS --noproxy "*" --max-time 5 "${DAEMON_URL}/api/v1/workflow/stats")"
  done_cnt="$(python3 - <<PY
import json
obj = json.loads(r'''${stats}''')
wb = obj.get("workflows_by_status") or {}
print(int(wb.get("done") or 0))
PY
)"
  if [[ "${done_cnt}" -ge 5 ]]; then
    break
  fi
  sleep 0.02
done
if [[ "${done_cnt}" -lt 5 ]]; then
  echo "timed out waiting for done_cnt>=5 (got ${done_cnt}); stats=${stats}" >&2
  exit 1
fi

python3 - <<PY
import json, subprocess, sys

daemon_url = r"""${DAEMON_URL}"""
wids = r"""${WIDS[*]}""".split()
if len(wids) != 5:
  print("expected WIDS=5", wids, file=sys.stderr)
  raise SystemExit(1)

rows = []
for wid in wids:
  raw = subprocess.check_output([
    "curl", "-fsS", "--noproxy", "*", "--max-time", "10",
    f"{daemon_url}/api/v1/workflow?workflow_id={wid}&include_tasks=1&include_results=1"
  ], text=True)
  obj = json.loads(raw)
  if not obj.get("ok"):
    print("workflow get failed", wid, obj, file=sys.stderr)
    raise SystemExit(1)
  wf = obj.get("workflow") or {}
  if wf.get("status") != "done":
    print("workflow not done", wid, wf, file=sys.stderr)
    raise SystemExit(1)
  sid = wf.get("session_id","")
  updated = int(wf.get("updated_unix_ms") or 0)
  tasks = obj.get("tasks") or []
  if len(tasks) != 1:
    print("expected 1 task", wid, tasks, file=sys.stderr)
    raise SystemExit(1)
  res = (tasks[0].get("result") or {})
  tag = res.get("assistant_text") or ""
  if not tag:
    print("missing assistant_text tag", wid, res, file=sys.stderr)
    raise SystemExit(1)
  want_sid = "sess_A" if tag.startswith("A") else ("sess_B" if tag.startswith("B") else "")
  if want_sid and sid != want_sid:
    print("unexpected workflow.session_id", sid, "want", want_sid, "wid", wid, file=sys.stderr)
    print("wf", wf, file=sys.stderr)
    raise SystemExit(1)
  rows.append((updated, wid, tag))

rows.sort()
seq = "".join(("A" if r[2].startswith("A") else ("B" if r[2].startswith("B") else "?")) for r in rows)
tags = [r[2] for r in rows]

if seq.count("A") != 3 or seq.count("B") != 2 or "?" in seq:
  print("unexpected completion mix; got", seq, file=sys.stderr)
  print("tags_by_finish", tags, file=sys.stderr)
  raise SystemExit(1)

# Minimal deterministic fairness + weight signal:
# - B must appear early (not starved behind all A workflows)
# - A should dominate the early prefix (weight=2)
if "B" not in seq[:3]:
  print("expected a B completion within first 3 (avoid starvation); got", seq, file=sys.stderr)
  print("tags_by_finish", tags, file=sys.stderr)
  raise SystemExit(1)
if seq[:3].count("A") < 2:
  print("expected at least two A completions within first 3 (weight=2); got", seq, file=sys.stderr)
  print("tags_by_finish", tags, file=sys.stderr)
  raise SystemExit(1)

print("ok: weighted RR interleaving observed:", seq, tags)
PY
