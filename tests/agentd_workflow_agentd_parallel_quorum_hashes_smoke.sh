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

HOST="127.0.0.1"
PORT_REMOTE_A="$(agentd_smoke_pick_port)"
PORT_REMOTE_B="$(agentd_smoke_pick_port)"
PORT_REMOTE_C="$(agentd_smoke_pick_port)"
PORT_LOCAL="$(agentd_smoke_pick_port)"
NAME="agentd_workflow_agentd_parallel_quorum_hashes_smoke"

cleanup() {
  # Stop local (agentd_smoke_stop uses AGENTD_PID global).
  agentd_smoke_stop

  for pid in ${REMOTE_PIDS:-}; do
    kill -TERM "${pid}" >/dev/null 2>&1 || true
  done
  for pid in ${REMOTE_PIDS:-}; do
    for _ in $(seq 1 30); do
      if ! kill -0 "${pid}" >/dev/null 2>&1; then
        break
      fi
      sleep 0.1
    done
    if kill -0 "${pid}" >/dev/null 2>&1; then
      kill -KILL "${pid}" >/dev/null 2>&1 || true
    fi
    wait "${pid}" >/dev/null 2>&1 || true
  done
}
trap cleanup EXIT

# Start three remote agentd instances (collaboration targets).
REMOTE_PIDS=""
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_REMOTE_A}" "${NAME}_remote_a" --tools none
REMOTE_PIDS="${REMOTE_PIDS} ${AGENTD_PID}"
REMOTE_URL_A="${DAEMON_URL}"
agentd_smoke_wait_health "${REMOTE_URL_A}"

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_REMOTE_B}" "${NAME}_remote_b" --tools none
REMOTE_PIDS="${REMOTE_PIDS} ${AGENTD_PID}"
REMOTE_URL_B="${DAEMON_URL}"
agentd_smoke_wait_health "${REMOTE_URL_B}"

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_REMOTE_C}" "${NAME}_remote_c" --tools none
REMOTE_PIDS="${REMOTE_PIDS} ${AGENTD_PID}"
REMOTE_URL_C="${DAEMON_URL}"
agentd_smoke_wait_health "${REMOTE_URL_C}"

# Start local agentd (runs the workflow with kind:"agentd_parallel").
#
# Enable strict outbound policy to keep the test robust if defaults change:
# - enable http tasks
# - deny private by default, but explicitly allow 127.0.0.0/8 for local test daemons
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_LOCAL}" "${NAME}_local" \
  --tools none \
  --workflow-enable-http-tasks \
  --workflow-http-deny-private \
  --workflow-http-allow-cidr 127.0.0.0/8
LOCAL_URL="${DAEMON_URL}"
agentd_smoke_wait_health "${LOCAL_URL}"

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
tasks = [
  {
    "task_id": "P",
    "kind": "agentd_parallel",
    "agentd_parallel": {
      "targets": [
        {"id": "a", "base_url": "${REMOTE_URL_A}", "allow_error": True},
        {"id": "b", "base_url": "${REMOTE_URL_B}", "allow_error": True},
        {"id": "c", "base_url": "${REMOTE_URL_C}", "allow_error": True}
      ],
      "agentd_call": {
        "op": "workflow_submit_and_wait",
        "timeout_ms": 20000,
        "poll_ms": 20,
        "max_bytes": 1048576,
        "include_tasks": False,
        "include_results": True,
        "workflow": {
          "tasks": [
            {"task_id": "W", "kind": "delay", "delay_ms": 10, "result": {"assistant_text": "remote ok"}}
          ]
        }
      },
      "aggregate": {
        "mode": "quorum_hashes",
        "quorum": 2,
        "pointers": [
          "/agentd/final/result/results_by_task/W/assistant_text",
          "/agentd/result_sha256"
        ]
      }
    }
  }
]
print(json.dumps({"tasks": tasks}))
PY
)" \
  "${LOCAL_URL}/api/v1/workflow/submit")"

workflow_id="$(python3 - <<PY
import json
obj = json.loads(r'''${submit_resp}''')
print(obj.get("workflow_id",""))
PY
)"
if [[ -z "${workflow_id}" ]]; then
  echo "failed to get workflow_id: ${submit_resp}" >&2
  exit 1
fi

final=""
for _ in $(seq 1 500); do
  final="$(curl -fsS --noproxy "*" --max-time 5 \
    "${LOCAL_URL}/api/v1/workflow?workflow_id=${workflow_id}&include_tasks=1&include_results=1")"
  if python3 - <<PY >/dev/null 2>&1
import json
obj = json.loads(r'''${final}''')
w = obj.get("workflow") or {}
st = w.get("status")
raise SystemExit(0 if st in ("done","error","cancelled") else 1)
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
  print("expected workflow status done", w, file=sys.stderr)
  raise SystemExit(1)

tasks = obj.get("tasks") or []
by_id = {t.get("task_id"): t for t in tasks if isinstance(t, dict) and isinstance(t.get("task_id"), str)}
if by_id.get("P", {}).get("status") != "done":
  print("expected join task P done", by_id.get("P"), file=sys.stderr)
  raise SystemExit(1)

res = obj.get("result") or {}
by = res.get("results_by_task") or {}
j = by.get("P") or {}
if j.get("mode") != "quorum_hashes" or j.get("ok") is not True:
  print("unexpected join result", j, file=sys.stderr)
  raise SystemExit(1)

checks = j.get("checks") or []
ptrs = {c.get("ptr"): c for c in checks if isinstance(c, dict)}

c1 = ptrs.get("/agentd/final/result/results_by_task/W/assistant_text") or {}
if c1.get("ok") is not True:
  print("expected quorum ok for assistant_text pointer", c1, file=sys.stderr)
  raise SystemExit(1)
if (c1.get("chosen") or "").strip() != "remote ok":
  print("unexpected chosen assistant_text", c1, file=sys.stderr)
  raise SystemExit(1)

c2 = ptrs.get("/agentd/result_sha256") or {}
if c2.get("ok") is not True:
  print("expected quorum ok for result_sha256 pointer", c2, file=sys.stderr)
  raise SystemExit(1)
chosen = c2.get("chosen") or ""
if not (isinstance(chosen, str) and chosen.startswith("sha256:") and len(chosen) == 71):
  print("unexpected chosen result_sha256", chosen, file=sys.stderr)
  raise SystemExit(1)
PY

echo "${NAME} OK"

