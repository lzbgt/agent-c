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
PORT_DAEMON="$(agentd_smoke_pick_port)"
AUTH_TOKEN="agentd_workflow_aggregate_quorum_smoke_token"
SESSION_ID="sess_workflow_aggregate_quorum_smoke"

trap agentd_smoke_stop EXIT

LOG_DIR="$(agentd_smoke_log_dir)"
TMP_DIR="${LOG_DIR}/agentd_workflow_aggregate_quorum_smoke_${PORT_DAEMON}.tmp"
mkdir -p "${TMP_DIR}"

# Deterministic AVM stub runner.
AVM_STUB_BIN="${TMP_DIR}/avm_stub.sh"
cat > "${AVM_STUB_BIN}" <<'SH'
#!/usr/bin/env bash
set -euo pipefail

mode=""
file=""

args=("$@")
i=0
while [[ $i -lt ${#args[@]} ]]; do
  a="${args[$i]}"
  case "${a}" in
    --timeout-ms|--allow-domains)
      i=$((i + 2))
      continue
      ;;
    --capsule|--print-result-hash|--print-trace-hash|--print-state-hash)
      i=$((i + 1))
      continue
      ;;
    --print-run-json)
      mode="--print-run-json"
      i=$((i + 1))
      continue
      ;;
    *)
      if [[ -z "${file}" && "${a}" != -* ]]; then
        file="${a}"
      fi
      i=$((i + 1))
      continue
      ;;
  esac
done

if [[ -z "${file}" || ! -f "${file}" ]]; then
  echo "missing file" >&2
  exit 2
fi

case "${mode}" in
  --print-run-json)
    echo '{"schema":"avm.run.v1","exit_code":0,"gas_executed":10,"wall_elapsed_ns":1000,"has_result":true,"result_type":"string","result":"ok"}'
    echo "RESULT_HASH stubresulthash"
    echo "TRACE_HASH stubtracehash"
    echo "STATE_HASH stubstatehash"
    ;;
  *)
    echo "unexpected argv (mode not found)" >&2
    exit 2
    ;;
esac
SH
chmod +x "${AVM_STUB_BIN}"

export AGENTD_AVM_BIN="${AVM_STUB_BIN}"
export AGENTD_AVM_EXEC=1
export AGENTD_NODE_ID="agentd-avm-quorum-smoke-node"

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_workflow_aggregate_quorum_smoke" \
  --auth-token "${AUTH_TOKEN}" \
  --yolo \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

obc_bytes="hello_avm_obc_quorum"
obc_b64="$(python3 - <<PY
import base64
print(base64.b64encode(b"${obc_bytes}").decode("ascii"))
PY
)"

submit_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Authorization: Bearer ${AUTH_TOKEN}" \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
cap = {"obc_base64":"${obc_b64}","timeout_ms":1000,"gas":1000,"mem_bytes":64000,"io_bytes":0,"log_bytes":0,"deterministic":True}
tasks = [
  {"task_id":"AVM1","kind":"avm_capsule","capsule":cap},
  {"task_id":"AVM2","kind":"avm_capsule","capsule":cap},
  {"task_id":"AVM3","kind":"avm_capsule","capsule":cap},
  {"task_id":"J","kind":"aggregate","depends_on":["AVM1","AVM2","AVM3"],
   "aggregate":{"mode":"quorum_hashes","task_ids":["AVM1","AVM2","AVM3"],"quorum":2,"pointers":["/avm/result_hash","/avm/trace_hash"]}}
]
print(json.dumps({"tasks": tasks, "allow_sessions": True, "session_id": "${SESSION_ID}"}))
PY
)" \
  "${DAEMON_URL}/api/v1/workflow/submit")"

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
for _ in $(seq 1 200); do
  final="$(curl -fsS --noproxy "*" --max-time 5 \
    -H "Authorization: Bearer ${AUTH_TOKEN}" \
    "${DAEMON_URL}/api/v1/workflow?workflow_id=${workflow_id}&include_tasks=1&include_results=1")"
  if python3 - <<PY >/dev/null 2>&1
import json, sys
obj = json.loads(r'''${final}''')
w = obj.get("workflow") or {}
st = w.get("status")
if st in ("done","error","cancelled"):
  raise SystemExit(0)
raise SystemExit(1)
PY
  then
    break
  fi
  sleep 0.1
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

result = obj.get("result") or {}
by_task = result.get("results_by_task") or {}

rj = by_task.get("J") or {}
if rj.get("kind") != "aggregate":
  print("unexpected J kind", rj.get("kind"), file=sys.stderr)
  raise SystemExit(1)
if rj.get("ok") is not True:
  print("expected aggregate ok true", rj, file=sys.stderr)
  raise SystemExit(1)
if (rj.get("assistant_text") or "").strip() != "stubresulthash":
  print("unexpected aggregate assistant_text", rj.get("assistant_text"), file=sys.stderr)
  raise SystemExit(1)
if rj.get("node_pointer") != "/avm/attest/node_id":
  print("unexpected aggregate node_pointer", rj.get("node_pointer"), file=sys.stderr)
  raise SystemExit(1)
nodes = rj.get("nodes_by_task_id") or {}
expected_nodes = {
  "AVM1": "agentd-avm-quorum-smoke-node",
  "AVM2": "agentd-avm-quorum-smoke-node",
  "AVM3": "agentd-avm-quorum-smoke-node",
}
if nodes != expected_nodes:
  print("unexpected nodes_by_task_id", nodes, file=sys.stderr)
  raise SystemExit(1)
attestations = rj.get("attestations_by_task_id") or {}
for tid in ("AVM1", "AVM2", "AVM3"):
  att = attestations.get(tid) or {}
  if att.get("schema") != "run_attestation_bundle_v1":
    print("unexpected attestation schema", tid, att, file=sys.stderr)
    raise SystemExit(1)
  if att.get("node_id") != "agentd-avm-quorum-smoke-node":
    print("unexpected attestation node_id", tid, att, file=sys.stderr)
    raise SystemExit(1)
  if not str(att.get("replay_sha256") or "").startswith("sha256:"):
    print("unexpected attestation replay hash", tid, att, file=sys.stderr)
    raise SystemExit(1)

checks = rj.get("checks") or []
if len(checks) < 2:
  print("expected >=2 checks", checks, file=sys.stderr)
  raise SystemExit(1)

by_ptr = {c.get("ptr"): c for c in checks if isinstance(c, dict)}
rh = by_ptr.get("/avm/result_hash") or {}
th = by_ptr.get("/avm/trace_hash") or {}

if rh.get("chosen") != "stubresulthash" or rh.get("ok") is not True:
  print("unexpected result_hash check", rh, file=sys.stderr)
  raise SystemExit(1)
if th.get("chosen") != "stubtracehash" or th.get("ok") is not True:
  print("unexpected trace_hash check", th, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_workflow_aggregate_quorum_smoke OK"
