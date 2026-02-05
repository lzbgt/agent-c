#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
FIXTURE_PATH="${2:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "usage: agentd_edge_interop_task_loop_replay_smoke.sh <agentd_bin> [fixture_jsonl]" >&2
  exit 2
fi

if [[ -z "${FIXTURE_PATH}" ]]; then
  ROOT="$(agentd_smoke_project_root)"
  FIXTURE_PATH="${ROOT}/docs/spec/um-eais/fixtures/umbmp_task_loop_v0.1.jsonl"
fi
if [[ ! -f "${FIXTURE_PATH}" ]]; then
  echo "fixture not found: ${FIXTURE_PATH}" >&2
  exit 2
fi

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

PORT_DAEMON="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"
NAME="agentd_edge_interop_task_loop_replay_smoke"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "${NAME}" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

PRE_FIXTURE="${LOG_DIR}/${NAME}_${PORT_DAEMON}.pre.jsonl"
POST_FIXTURE="${LOG_DIR}/${NAME}_${PORT_DAEMON}.post.jsonl"

meta="$(python3 - <<PY
import json, sys
path = r'''${FIXTURE_PATH}'''
pre = []
post = []
node_id = ""
task_id = ""
step_id = ""
idem = ""
for line in open(path, "r", encoding="utf-8"):
  line = line.strip()
  if not line:
    continue
  env = json.loads(line)
  t = env.get("type") or ""
  if t in ("NODE_HELLO", "NODE_CAPS_RSP", "NODE_HEARTBEAT"):
    pre.append(line)
  elif t.startswith("TASK_") or t == "SENSOR_EVENT":
    post.append(line)
  else:
    # Ignore unknown types in this smoke.
    post.append(line)

  if t == "NODE_HELLO" and not node_id:
    node_id = ((env.get("body") or {}).get("node_id") or "").strip()
  if t in ("TASK_ACK", "TASK_EVENT", "TASK_DONE", "TASK_FAILED") and not task_id:
    b = env.get("body") or {}
    task_id = (b.get("task_id") or "").strip()
    step_id = (b.get("step_id") or "").strip()
    idem = (b.get("idempotency_key") or "").strip()

out = {
  "node_id": node_id,
  "task_id": task_id,
  "step_id": step_id,
  "idempotency_key": idem,
  "pre_lines": len(pre),
  "post_lines": len(post),
  "pre": "\\n".join(pre) + ("\\n" if pre else ""),
  "post": "\\n".join(post) + ("\\n" if post else ""),
}
print(json.dumps(out))
PY
)"

node_id="$(python3 - <<PY
import json
print(json.loads(r'''${meta}''')["node_id"])
PY
)"
task_id="$(python3 - <<PY
import json
print(json.loads(r'''${meta}''')["task_id"])
PY
)"
step_id="$(python3 - <<PY
import json
print(json.loads(r'''${meta}''')["step_id"])
PY
)"
idempotency_key="$(python3 - <<PY
import json
print(json.loads(r'''${meta}''')["idempotency_key"])
PY
)"

python3 - <<PY
import json
meta = json.loads(r'''${meta}''')
open(r'''${PRE_FIXTURE}''', "w", encoding="utf-8").write(meta["pre"])
open(r'''${POST_FIXTURE}''', "w", encoding="utf-8").write(meta["post"])
PY

if [[ -z "${node_id}" || -z "${task_id}" || -z "${step_id}" || -z "${idempotency_key}" ]]; then
  echo "failed to extract node/task ids from fixture: ${FIXTURE_PATH}" >&2
  exit 1
fi

while IFS= read -r line; do
  if [[ -z "${line//[[:space:]]/}" ]]; then
    continue
  fi
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/json" \
    -d "${line}" \
    "${DAEMON_URL}/api/v1/edge/message" >/dev/null
done < "${PRE_FIXTURE}"

assign_json="$(python3 - <<PY
import json, time
now = int(time.time() * 1000)
args = {
  "node_id": "${node_id}",
  "task_id": "${task_id}",
  "step_id": "${step_id}",
  "idempotency_key": "${idempotency_key}",
  "mode": "agent",
  "deadline_utc_ms": now + 60000,
  "payload": {"prompt": "fixture: task loop smoke"},
}
print(json.dumps(args))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${assign_json}" \
  "${DAEMON_URL}/api/v1/edge/task/assign" >/dev/null

# Poll outbox until TASK_ASSIGN is visible.
seen="0"
for _ in $(seq 1 120); do
  outbox="$(curl -fsS --noproxy "*" --max-time 10 \
    "${DAEMON_URL}/api/v1/edge/outbox?node_id=${node_id}&cursor=0&limit=400")"
  ok="$(python3 - <<PY
import json
obj = json.loads(r'''${outbox}''')
msgs = obj.get("messages") or []
for row in msgs:
  env = (row.get("msg") or {})
  if env.get("type") != "TASK_ASSIGN":
    continue
  body = env.get("body") or {}
  if body.get("task_id") == "${task_id}" and body.get("step_id") == "${step_id}":
    print("1")
    break
else:
  print("0")
PY
)"
  if [[ "${ok}" == "1" ]]; then
    seen="1"
    break
  fi
  sleep 0.05
done
if [[ "${seen}" != "1" ]]; then
  echo "TASK_ASSIGN not observed in outbox for node_id=${node_id}" >&2
  exit 1
fi

while IFS= read -r line; do
  if [[ -z "${line//[[:space:]]/}" ]]; then
    continue
  fi
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/json" \
    -d "${line}" \
    "${DAEMON_URL}/api/v1/edge/message" >/dev/null
done < "${POST_FIXTURE}"

# Poll task until it reaches SUCCEEDED and has the expected result.
final=""
for _ in $(seq 1 120); do
  final="$(curl -fsS --noproxy "*" --max-time 10 \
    "${DAEMON_URL}/api/v1/edge/task?task_id=${task_id}&step_id=${step_id}")"
  if python3 - <<PY >/dev/null 2>&1
import json, sys
obj = json.loads(r'''${final}''')
t = obj.get("task") or {}
if t.get("state") != "SUCCEEDED":
  raise SystemExit(1)
res = (t.get("result") or {}).get("data") or {}
raise SystemExit(0 if res.get("note") == "fixture_done" else 1)
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
  print("task get failed", obj, file=sys.stderr)
  raise SystemExit(1)
t = obj.get("task") or {}
if t.get("state") != "SUCCEEDED":
  print("expected SUCCEEDED", t, file=sys.stderr)
  raise SystemExit(1)
res = (t.get("result") or {}).get("data") or {}
if res.get("note") != "fixture_done":
  print("unexpected result", t.get("result"), file=sys.stderr)
  raise SystemExit(1)
print("ok")
PY

