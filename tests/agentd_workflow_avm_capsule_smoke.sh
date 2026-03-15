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
PORT_STUB="$(agentd_smoke_pick_port)"
AUTH_TOKEN="agentd_workflow_avm_capsule_smoke_token"
STUB_BASE="http://${HOST}:${PORT_STUB}/v1"

cleanup() {
  agentd_smoke_stop
  if [[ -n "${STUB_PID:-}" ]]; then
    kill -TERM "${STUB_PID}" >/dev/null 2>&1 || true
    wait "${STUB_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

LOG_DIR="$(agentd_smoke_log_dir)"
TMP_DIR="${LOG_DIR}/agentd_workflow_avm_capsule_smoke_${PORT_DAEMON}.tmp"
mkdir -p "${TMP_DIR}"

HOME_ROOT="${TMP_DIR}/home"
ALLOW_ROOT="${TMP_DIR}/allow_root"
OUTSIDE_ROOT="${TMP_DIR}/outside_root"
mkdir -p "${HOME_ROOT}/.config/agent" "${ALLOW_ROOT}/allowed" "${OUTSIDE_ROOT}/blocked"
cat > "${HOME_ROOT}/.config/agent/mount-allowlist.json" <<JSON
{
  "allowed_roots": [
    { "path": "${ALLOW_ROOT}", "readonly": false }
  ],
  "blocked_patterns": [],
  "non_main_readonly": true
}
JSON

export HOME="${HOME_ROOT}"
export EXPECTED_ALLOWED_HOST="${ALLOW_ROOT}/allowed"
export EXPECTED_ALLOWED_CONTAINER="/workspace/extra/allowed"
export EXPECTED_ALLOWED_MATCHED_ROOT="${ALLOW_ROOT}"
export EXPECTED_ALLOWED_READONLY="0"

# Deterministic AVM stub runner (out-of-process).
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
    mount_count="${AGENTD_AVM_MOUNT_COUNT:-0}"
    if [[ "${mount_count}" != "0" ]]; then
      if [[ "${mount_count}" != "1" ]]; then
        echo "unexpected mount_count=${mount_count}" >&2
        exit 2
      fi
      if [[ "${AGENTD_AVM_MOUNT_0_HOST_PATH:-}" != "${EXPECTED_ALLOWED_HOST:-}" ]]; then
        echo "unexpected host path ${AGENTD_AVM_MOUNT_0_HOST_PATH:-}" >&2
        exit 2
      fi
      if [[ "${AGENTD_AVM_MOUNT_0_CONTAINER_PATH:-}" != "${EXPECTED_ALLOWED_CONTAINER:-}" ]]; then
        echo "unexpected container path ${AGENTD_AVM_MOUNT_0_CONTAINER_PATH:-}" >&2
        exit 2
      fi
      if [[ "${AGENTD_AVM_MOUNT_0_MATCHED_ROOT:-}" != "${EXPECTED_ALLOWED_MATCHED_ROOT:-}" ]]; then
        echo "unexpected matched root ${AGENTD_AVM_MOUNT_0_MATCHED_ROOT:-}" >&2
        exit 2
      fi
      if [[ "${AGENTD_AVM_MOUNT_0_READONLY:-}" != "${EXPECTED_ALLOWED_READONLY:-}" ]]; then
        echo "unexpected readonly ${AGENTD_AVM_MOUNT_0_READONLY:-}" >&2
        exit 2
      fi
      if [[ "${AGENTD_AVM_HOST_EFFECT_FS:-}" != "1" ]]; then
        echo "unexpected host effect fs ${AGENTD_AVM_HOST_EFFECT_FS:-}" >&2
        exit 2
      fi
      if [[ "${AGENTD_AVM_HOST_EFFECT_PROC:-}" != "0" ]]; then
        echo "unexpected host effect proc ${AGENTD_AVM_HOST_EFFECT_PROC:-}" >&2
        exit 2
      fi
      if [[ "${AGENTD_AVM_HOST_EFFECT_NET:-}" != "0" ]]; then
        echo "unexpected host effect net ${AGENTD_AVM_HOST_EFFECT_NET:-}" >&2
        exit 2
      fi
      python3 - <<'PY'
import json, os, sys
mounts = json.loads(os.environ.get("AGENTD_AVM_MOUNTS_JSON", "[]"))
if len(mounts) != 1:
  print("unexpected mounts json", mounts, file=sys.stderr)
  raise SystemExit(1)
mount = mounts[0]
expected = {
  "host_path": os.environ["EXPECTED_ALLOWED_HOST"],
  "container_path": os.environ["EXPECTED_ALLOWED_CONTAINER"],
  "matched_root": os.environ["EXPECTED_ALLOWED_MATCHED_ROOT"],
  "readonly": False,
  "is_main": True,
}
for key, value in expected.items():
  if mount.get(key) != value:
    print(f"unexpected mount field {key}: {mount.get(key)!r}", file=sys.stderr)
    raise SystemExit(1)
PY
    fi
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
export AGENTD_AVM_ALLOW_FS=1

# OpenAI-compatible stub:
# - echoes user prompt as assistant content
python3 -u - <<PY > "${LOG_DIR}/agentd_workflow_avm_capsule_smoke.stub.stdout.log" 2> "${LOG_DIR}/agentd_workflow_avm_capsule_smoke.stub.stderr.log" &
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

def last_user_prompt(req):
  msgs = req.get("messages") or []
  for m in reversed(msgs):
    if isinstance(m, dict) and m.get("role") == "user":
      c = m.get("content")
      if isinstance(c, str):
        return c
  return ""

class H(BaseHTTPRequestHandler):
  def log_message(self, fmt, *args):
    return

  def do_POST(self):
    if self.path != "/v1/chat/completions":
      self.send_response(404)
      self.end_headers()
      return
    raw = self.rfile.read(int(self.headers.get("Content-Length","0") or "0"))
    try:
      req = json.loads(raw.decode("utf-8"))
    except Exception:
      self.send_response(400)
      self.end_headers()
      return

    prompt = last_user_prompt(req)
    body = {
      "id": "cmpl_stub",
      "object": "chat.completion",
      "created": 0,
      "model": "stub",
      "choices": [
        {"index": 0, "message": {"role": "assistant", "content": prompt}, "finish_reason": "stop"}
      ],
    }
    data = json.dumps(body).encode("utf-8")
    self.send_response(200)
    self.send_header("Content-Type", "application/json; charset=utf-8")
    self.send_header("Content-Length", str(len(data)))
    self.end_headers()
    self.wfile.write(data)

ThreadingHTTPServer(("127.0.0.1", ${PORT_STUB}), H).serve_forever()
PY
STUB_PID=$!

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_workflow_avm_capsule_smoke" \
  --auth-token "${AUTH_TOKEN}" \
  --yolo \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

obc_bytes="hello_avm_obc_workflow"
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
tasks = [
  {"task_id":"AVM","kind":"avm_capsule","capsule":{"obc_base64":"${obc_b64}","timeout_ms":1000,"gas":1000,"mem_bytes":64000,"io_bytes":0,"log_bytes":0,"deterministic":True,"host_effects":{"fs":True},"mounts":[{"host_path":"${ALLOW_ROOT}/allowed","container_path":"/workspace/extra/allowed","is_main":True}]}},
  {"task_id":"B","depends_on":["AVM"],"request":{"prompt":"B got \${task.AVM.json:/assistant_text}","no_session":True,"tools":"none","base_url":"${STUB_BASE}","api_key":"dummy","model":"stub","trace":False}},
]
print(json.dumps({"tasks": tasks, "allow_inline_api_keys": True}))
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

tasks = obj.get("tasks") or []
by_id = {t.get("task_id"): t for t in tasks if isinstance(t, dict)}
for tid in ("AVM","B"):
  if by_id.get(tid, {}).get("status") != "done":
    print("task not done", tid, by_id.get(tid), file=sys.stderr)
    raise SystemExit(1)

result = obj.get("result") or {}
by_task = result.get("results_by_task") or {}
r_avm = by_task.get("AVM") or {}

if r_avm.get("kind") != "avm_capsule":
  print("unexpected AVM kind", r_avm.get("kind"), file=sys.stderr)
  raise SystemExit(1)
if r_avm.get("ok") is not True:
  print("expected AVM ok true", r_avm, file=sys.stderr)
  raise SystemExit(1)
avm = r_avm.get("avm") or {}
run = avm.get("run") or {}
if run.get("schema") != "avm.run.v1":
  print("unexpected avm.run.schema", run.get("schema"), file=sys.stderr)
  raise SystemExit(1)
if avm.get("run_json_raw", "").find('"schema":"avm.run.v1"') == -1:
  print("missing avm.run_json_raw", avm.get("run_json_raw"), file=sys.stderr)
  raise SystemExit(1)
mounts = avm.get("mounts") or []
if len(mounts) != 1:
  print("unexpected AVM mounts", mounts, file=sys.stderr)
  raise SystemExit(1)
mount = mounts[0]
if mount.get("host_path") != "${ALLOW_ROOT}/allowed":
  print("unexpected AVM mount host_path", mount.get("host_path"), file=sys.stderr)
  raise SystemExit(1)
if mount.get("container_path") != "/workspace/extra/allowed":
  print("unexpected AVM mount container_path", mount.get("container_path"), file=sys.stderr)
  raise SystemExit(1)
if mount.get("readonly") is not False:
  print("unexpected AVM mount readonly", mount.get("readonly"), file=sys.stderr)
  raise SystemExit(1)
if avm.get("result_hash") != "stubresulthash":
  print("unexpected result_hash", avm.get("result_hash"), file=sys.stderr)
  raise SystemExit(1)
if avm.get("trace_hash") != "stubtracehash":
  print("unexpected trace_hash", avm.get("trace_hash"), file=sys.stderr)
  raise SystemExit(1)
if avm.get("state_hash") != "stubstatehash":
  print("unexpected state_hash", avm.get("state_hash"), file=sys.stderr)
  raise SystemExit(1)
out = avm.get("output") or {}
hashes = out.get("hashes") or {}
if hashes.get("result_hash") != "stubresulthash" or hashes.get("trace_hash") != "stubtracehash" or hashes.get("state_hash") != "stubstatehash":
  print("unexpected avm.output.hashes", hashes, file=sys.stderr)
  raise SystemExit(1)
if "RESULT_HASH stubresulthash" not in (out.get("residual_text") or ""):
  print("missing avm.output.residual_text hash lines", out, file=sys.stderr)
  raise SystemExit(1)
host_effects = avm.get("host_effects") or {}
if host_effects.get("fs") is not True or host_effects.get("proc") is not False or host_effects.get("net") is not False:
  print("unexpected AVM host_effects", host_effects, file=sys.stderr)
  raise SystemExit(1)

if (r_avm.get("assistant_text") or "").strip() != "stubresulthash":
  print("unexpected AVM assistant_text", r_avm.get("assistant_text"), file=sys.stderr)
  raise SystemExit(1)

r_b = by_task.get("B") or {}
if "B got stubresulthash" not in (r_b.get("assistant_text") or ""):
  print("unexpected B assistant_text", r_b.get("assistant_text"), file=sys.stderr)
  raise SystemExit(1)
PY

submit_bad_resp="$(curl -fsS --noproxy "*" --max-time 20 \
  -H "Authorization: Bearer ${AUTH_TOKEN}" \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
tasks = [
  {"task_id":"AVM_BAD","kind":"avm_capsule","capsule":{"obc_base64":"${obc_b64}","timeout_ms":1000,"host_effects":{"fs":True},"mounts":[{"host_path":"${OUTSIDE_ROOT}/blocked","container_path":"/workspace/extra/blocked","is_main":True}]}}
]
print(json.dumps({"tasks": tasks, "allow_inline_api_keys": True}))
PY
)" \
  "${DAEMON_URL}/api/v1/workflow/submit")"

workflow_bad_id="$(python3 - <<PY
import json
obj = json.loads(r'''${submit_bad_resp}''')
print(obj.get("workflow_id",""))
PY
)"
if [[ -z "${workflow_bad_id}" ]]; then
  echo "failed to get bad workflow_id: ${submit_bad_resp}" >&2
  exit 1
fi

final_bad=""
for _ in $(seq 1 200); do
  final_bad="$(curl -fsS --noproxy "*" --max-time 5 \
    -H "Authorization: Bearer ${AUTH_TOKEN}" \
    "${DAEMON_URL}/api/v1/workflow?workflow_id=${workflow_bad_id}&include_tasks=1&include_results=1")"
  if python3 - <<PY >/dev/null 2>&1
import json, sys
obj = json.loads(r'''${final_bad}''')
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
obj = json.loads(r'''${final_bad}''')
if not obj.get("ok"):
  print("workflow get failed", obj, file=sys.stderr)
  raise SystemExit(1)
w = obj.get("workflow") or {}
if w.get("status") != "error":
  print("expected workflow status error", w, file=sys.stderr)
  raise SystemExit(1)
tasks = obj.get("tasks") or []
by_id = {t.get("task_id"): t for t in tasks if isinstance(t, dict)}
task = by_id.get("AVM_BAD") or {}
if task.get("status") != "error":
  print("expected AVM_BAD task error", task, file=sys.stderr)
  raise SystemExit(1)
result = obj.get("result") or {}
by_task = result.get("results_by_task") or {}
r_bad = by_task.get("AVM_BAD") or {}
if r_bad.get("ok") is not False:
  print("expected AVM_BAD result ok=false", r_bad, file=sys.stderr)
  raise SystemExit(1)
err = (r_bad.get("error") or "") + " " + ((r_bad.get("avm") or {}).get("error") or "")
if "host_path_outside_roots" not in err:
  print("unexpected AVM_BAD error", err, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_workflow_avm_capsule_smoke OK"
