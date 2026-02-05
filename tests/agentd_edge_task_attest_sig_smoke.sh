#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
ED_TOOL="${2:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "missing agentd binary path arg" >&2
  exit 2
fi
if [[ -z "${ED_TOOL}" ]]; then
  echo "missing agent_ed25519_tool binary path arg" >&2
  exit 2
fi

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

PORT_DAEMON="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"

DB_PATH="${LOG_DIR}/agentd_edge_task_attest_sig_smoke_${PORT_DAEMON}.sqlite"
STATE_DIR="${LOG_DIR}/agentd_edge_task_attest_sig_smoke_${PORT_DAEMON}.state"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_task_attest_sig_smoke" \
  --db-path "${DB_PATH}" \
  --state-dir "${STATE_DIR}" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

TASK_ID="t_attest_sig_$(date +%s)_$RANDOM"
STEP_ID="s1"
NODE_ID="node_attest_sig_1"
IDEM="k_attest_sig_1"
TRACE_ID="trace_attest_sig_${TASK_ID}"

# Deterministic seed for ed25519 (RFC8032 vector #1 seed).
SK_HEX="9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"
KID="${NODE_ID}"

PK_B64="$("${ED_TOOL}" --sk-hex "${SK_HEX}" --print-pk-b64 | tr -d '\r\n')"
if [[ -z "${PK_B64}" ]]; then
  echo "failed to compute pk b64" >&2
  exit 1
fi

# Provision pubkey (used for verifying result attest signatures).
curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(python3 - <<PY
import json
print(json.dumps({
  "edge_auth_ed25519_pubkeys": {"${KID}": "${PK_B64}"},
}))
PY
)" \
  "${DAEMON_URL}/api/v1/config/update" >/dev/null

# Assign a task so the platform has an edge_tasks row to update.
assign_resp="$(curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "$(TASK_ID="${TASK_ID}" STEP_ID="${STEP_ID}" NODE_ID="${NODE_ID}" IDEM="${IDEM}" TRACE_ID="${TRACE_ID}" python3 - <<'PY'
import json, os, time
now = int(time.time() * 1000)
payload = {
  "node_id": os.environ.get("NODE_ID") or "",
  "task_id": os.environ.get("TASK_ID") or "",
  "step_id": os.environ.get("STEP_ID") or "",
  "idempotency_key": os.environ.get("IDEM") or "",
  "mode": "agent",
  "deadline_utc_ms": now + 60000,
  "attempt": 0,
  "payload": {"prompt": "ping"},
  "trace": {"trace_id": os.environ.get("TRACE_ID") or ""},
}
print(json.dumps(payload))
PY
)" \
  "${DAEMON_URL}/api/v1/edge/task/assign")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${assign_resp}''')
if not obj.get("ok"):
  print("assign failed", obj, file=sys.stderr)
  raise SystemExit(1)
PY

# Build a TASK_DONE with a signed attest blob over result_sha256.
done_resp="$(ED25519_TOOL="${ED_TOOL}" python3 - <<PY
import hashlib, json, os, subprocess, time, uuid

tool = os.environ["ED25519_TOOL"]
sk_hex = "${SK_HEX}"

now = int(time.time() * 1000)
result = {"ok": True, "assistant_text": "ok"}
canon = json.dumps(result, sort_keys=True, separators=(",", ":")).encode("utf-8")
result_sha = "sha256:" + hashlib.sha256(canon).hexdigest()

sig_input = (
  "UM_EAIS_RESULT_ATTEST_v0_1\\n"
  + "${TASK_ID}" + "\\n"
  + "${STEP_ID}" + "\\n"
  + "${IDEM}" + "\\n"
  + result_sha + "\\n"
  + str(now) + "\\n"
).encode("utf-8")

sig_b64 = subprocess.check_output([tool, "--sk-hex", sk_hex], input=sig_input).decode("utf-8").strip()

env = {
  "msg_id": "m_" + uuid.uuid4().hex,
  "ts_utc_ms": now,
  "type": "TASK_DONE",
  "from": "node:" + "${NODE_ID}",
  "to": "platform",
  "trace": {"trace_id": "${TRACE_ID}"},
  "body": {
    "task_id": "${TASK_ID}",
    "step_id": "${STEP_ID}",
    "idempotency_key": "${IDEM}",
    "result": {
      **result,
      "attest": {
        "result_sha256": result_sha,
        "kid": "${KID}",
        "alg": "ed25519",
        "sig": sig_b64,
        "ts_utc_ms": now,
      },
    },
  },
}
print(json.dumps(env))
PY
)"

curl -fsS --noproxy "*" --max-time 10 \
  -H "Content-Type: application/json" \
  -d "${done_resp}" \
  "${DAEMON_URL}/api/v1/edge/message" >/dev/null

# Platform should compute the same portable result_sha256 and also record _attest_sig_ok evidence in task events.
task_get="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/task?task_id=${TASK_ID}&step_id=${STEP_ID}")"

python3 - <<PY
import hashlib, json, re, sys
obj = json.loads(r'''${task_get}''')
if not obj.get("ok"):
  print("edge task get failed", obj, file=sys.stderr)
  raise SystemExit(1)
t = obj.get("task") or {}
if t.get("state") != "SUCCEEDED":
  print("expected SUCCEEDED got", t.get("state"), file=sys.stderr)
  raise SystemExit(1)
rsha = t.get("result_sha256") or ""
if not re.fullmatch(r"sha256:[0-9a-f]{64}", rsha):
  print("missing/invalid result_sha256", rsha, file=sys.stderr)
  raise SystemExit(1)
expected = "sha256:" + hashlib.sha256(json.dumps({"assistant_text":"ok","ok":True}, sort_keys=True, separators=(",", ":")).encode("utf-8")).hexdigest()
if rsha != expected:
  print("platform result_sha256 mismatch", rsha, expected, file=sys.stderr)
  raise SystemExit(1)
att = t.get("attest") or {}
if att.get("kid") != "${KID}" or att.get("alg") != "ed25519":
  print("attest metadata mismatch", att, file=sys.stderr)
  raise SystemExit(1)
if att.get("result_sha256") != expected:
  print("attest.result_sha256 mismatch", att.get("result_sha256"), expected, file=sys.stderr)
  raise SystemExit(1)
PY

trace_get="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/trace?trace_id=${TRACE_ID}&limit=200")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${trace_get}''')
if not obj.get("ok"):
  print("trace get failed", obj, file=sys.stderr)
  raise SystemExit(1)
recs = obj.get("records") or []
ok = False
for r in recs:
  if r.get("source") != "edge_task_event":
    continue
  if r.get("task_id") != "${TASK_ID}" or r.get("step_id") != "${STEP_ID}":
    continue
  ev = (r.get("event") or {})
  if ev.get("_attest_sig_ok") is True and ev.get("_attest_sig_kid") == "${KID}":
    ok = True
    break
if not ok:
  print("expected _attest_sig_ok evidence not found in trace records", recs, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_edge_task_attest_sig_smoke OK"

