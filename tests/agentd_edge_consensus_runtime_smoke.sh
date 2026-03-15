#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "usage: $0 <agentd_bin>" >&2
  exit 2
fi

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

PORT_DAEMON="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"
DAEMON_TOKEN="agentd-edge-consensus-runtime-token"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

AGENTD_AUTH_TOKEN="${DAEMON_TOKEN}" \
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_consensus_runtime_smoke" \
  --tools none
DAEMON_URL="http://${HOST}:${PORT_DAEMON}"
agentd_smoke_wait_health "${DAEMON_URL}" "${DAEMON_TOKEN}"

curl_json() {
  local method="$1"
  local path="$2"
  local body="${3:-}"
  if [[ -n "${body}" ]]; then
    curl -fsS --noproxy "*" --max-time 15 \
      -X "${method}" \
      -H "Authorization: Bearer ${DAEMON_TOKEN}" \
      -H 'Content-Type: application/json' \
      -d "${body}" \
      "${DAEMON_URL}${path}"
  else
    curl -fsS --noproxy "*" --max-time 15 \
      -X "${method}" \
      -H "Authorization: Bearer ${DAEMON_TOKEN}" \
      "${DAEMON_URL}${path}"
  fi
}

wait_runtime_done() {
  local node_id="$1"
  local status_json=""
  for _ in $(seq 1 200); do
    status_json="$(curl_json GET "/api/v1/edge/node/consensus_runtime?node_id=${node_id}")"
    if python3 - <<PY
import json
obj = json.loads(r'''${status_json}''')
rt = obj.get("runtime")
if not isinstance(rt, dict) or rt.get("running"):
  raise SystemExit(1)
res = rt.get("result") or {}
if not isinstance(res, dict) or not res.get("ok"):
  raise SystemExit(1)
PY
    then
      echo "${status_json}"
      return 0
    fi
    sleep 0.1
  done
  echo "timed out waiting for runtime ${node_id}" >&2
  echo "${status_json}" >&2
  return 1
}

wait_runtime_running() {
  local node_id="$1"
  local status_json=""
  for _ in $(seq 1 80); do
    status_json="$(curl_json GET "/api/v1/edge/node/consensus_runtime?node_id=${node_id}")"
    if python3 - <<PY
import json
obj = json.loads(r'''${status_json}''')
rt = obj.get("runtime")
if not isinstance(rt, dict) or not rt.get("running"):
  raise SystemExit(1)
PY
    then
      echo "${status_json}"
      return 0
    fi
    sleep 0.1
  done
  echo "timed out waiting for runtime ${node_id} to start" >&2
  echo "${status_json}" >&2
  return 1
}

NODE_A="node_runtime_cons_a"
NODE_B="node_runtime_cons_b"
NODE_C="node_runtime_cons_c"
CLUSTER_ID="lab-consensus-managed"
CAPS_SHA_A="sha256:1111111111111111111111111111111111111111111111111111111111111111"
CAPS_SHA_B="sha256:2222222222222222222222222222222222222222222222222222222222222222"
CAPS_SHA_C="sha256:3333333333333333333333333333333333333333333333333333333333333333"
DECISION_SHA="sha256:4444444444444444444444444444444444444444444444444444444444444444"

START_A_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"start","node_id":"${NODE_A}","cluster_id":"${CLUSTER_ID}","manifest_sha256":"${CAPS_SHA_A}","peer_node_ids":["${NODE_B}","${NODE_C}"],"member_node_ids":["${NODE_A}","${NODE_B}","${NODE_C}"],"membership_epoch":12,"decision_sha256":"${DECISION_SHA}","campaign_delay_ms":200,"campaign_retry_ms":200,"campaign_retry_max_ms":800,"campaign_retry_backoff_factor":2,"leader_heartbeat_ms":250,"leader_lease_ms":900,"trust_roots_epoch":9,"revocations_epoch":4,"cert_roots_epoch":11,"deadline_ms":12000}
JSON
)")"
RUNNING_A_JSON="$(wait_runtime_running "${NODE_A}")"

sleep 1.2

START_B_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"start","node_id":"${NODE_B}","cluster_id":"${CLUSTER_ID}","manifest_sha256":"${CAPS_SHA_B}","peer_node_ids":["${NODE_A}","${NODE_C}"],"member_node_ids":["${NODE_A}","${NODE_B}","${NODE_C}"],"membership_epoch":12,"trust_roots_epoch":9,"revocations_epoch":4,"cert_roots_epoch":11,"deadline_ms":12000}
JSON
)")"
START_C_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"start","node_id":"${NODE_C}","cluster_id":"${CLUSTER_ID}","manifest_sha256":"${CAPS_SHA_C}","peer_node_ids":["${NODE_A}","${NODE_B}"],"member_node_ids":["${NODE_A}","${NODE_B}","${NODE_C}"],"membership_epoch":12,"trust_roots_epoch":9,"revocations_epoch":4,"cert_roots_epoch":11,"deadline_ms":12000}
JSON
)")"

STATUS_A_JSON="$(wait_runtime_done "${NODE_A}")"
STATUS_B_JSON="$(wait_runtime_done "${NODE_B}")"
STATUS_C_JSON="$(wait_runtime_done "${NODE_C}")"
NODE_A_JSON="$(curl_json GET "/api/v1/edge/node?node_id=${NODE_A}")"
NODES_JSON="$(curl_json GET "/api/v1/edge/nodes?limit=10")"

STOP_NODE="node_runtime_cons_stop"
STOP_CLUSTER="lab-consensus-managed-stop"
STOP_SHA="sha256:5555555555555555555555555555555555555555555555555555555555555555"

START_STOP_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"start","node_id":"${STOP_NODE}","cluster_id":"${STOP_CLUSTER}","manifest_sha256":"${STOP_SHA}","peer_node_ids":["${NODE_A}"],"deadline_ms":30000,"poll_interval_ms":100}
JSON
)")"
RUNNING_STOP_JSON="$(wait_runtime_running "${STOP_NODE}")"
STOP_RESP_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"stop","node_id":"${STOP_NODE}"}
JSON
)")"
STOP_STATUS_JSON="$(curl_json GET "/api/v1/edge/node/consensus_runtime?node_id=${STOP_NODE}")"

python3 - <<PY
import json, sys

decision_sha = "${DECISION_SHA}"
member_ids = {"${NODE_A}", "${NODE_B}", "${NODE_C}"}

for label, raw in (
  ("running_a", r'''${RUNNING_A_JSON}'''),
  ("start_b", r'''${START_B_JSON}'''),
  ("start_c", r'''${START_C_JSON}'''),
  ("start_a", r'''${START_A_JSON}'''),
  ("start_stop", r'''${START_STOP_JSON}'''),
):
  obj = json.loads(raw)
  if not obj.get("ok") or not isinstance(obj.get("runtime"), dict):
    print(label, obj, file=sys.stderr)
    raise SystemExit(1)
  if (obj.get("runtime") or {}).get("runtime_kind") != "builtin":
    print(label, "runtime_kind not builtin", obj, file=sys.stderr)
    raise SystemExit(1)

if not (json.loads(r'''${RUNNING_A_JSON}''').get("runtime") or {}).get("running"):
  print("start_a never reached running state", json.loads(r'''${RUNNING_A_JSON}'''), file=sys.stderr)
  raise SystemExit(1)

results = {}
for label, raw in (
  ("A", r'''${STATUS_A_JSON}'''),
  ("B", r'''${STATUS_B_JSON}'''),
  ("C", r'''${STATUS_C_JSON}'''),
):
  obj = json.loads(raw)
  rt = obj.get("runtime") or {}
  res = rt.get("result") or {}
  if rt.get("running"):
    print("runtime still running", label, obj, file=sys.stderr)
    raise SystemExit(1)
  if not res.get("ok"):
    print("runtime missing successful result", label, obj, file=sys.stderr)
    raise SystemExit(1)
  if res.get("committed_decision_sha256") != decision_sha:
    print("wrong decision", label, obj, file=sys.stderr)
    raise SystemExit(1)
  results[label] = res

leader = results["A"].get("leader_node_id")
if leader not in member_ids:
  print("invalid elected leader", results, file=sys.stderr)
  raise SystemExit(1)
for label, res in results.items():
  if res.get("leader_node_id") != leader:
    print("wrong leader", label, res, file=sys.stderr)
    raise SystemExit(1)

status_a = (json.loads(r'''${STATUS_A_JSON}''').get("runtime") or {}).get("result") or {}
loop_status = status_a.get("status") or {}
if loop_status.get("campaign_attempts", 0) < 3:
  print("candidate A did not retry before quorum", status_a, file=sys.stderr)
  raise SystemExit(1)
if loop_status.get("leader_heartbeat_ms") != 250 or loop_status.get("leader_lease_ms") != 900:
  print("candidate A missing leader freshness config", status_a, file=sys.stderr)
  raise SystemExit(1)

node = (json.loads(r'''${NODE_A_JSON}''').get("node") or {})
rt = node.get("consensus_runtime") or {}
if rt.get("node_id") != "${NODE_A}":
  print("edge node missing runtime summary", node, file=sys.stderr)
  raise SystemExit(1)
if rt.get("runtime_kind") != "builtin":
  print("edge node runtime summary missing builtin kind", rt, file=sys.stderr)
  raise SystemExit(1)
if rt.get("campaign_retry_ms") != 200:
  print("runtime summary missing retry config", rt, file=sys.stderr)
  raise SystemExit(1)
if rt.get("campaign_retry_max_ms") != 800 or rt.get("campaign_retry_backoff_factor") != 2:
  print("runtime summary missing retry backoff config", rt, file=sys.stderr)
  raise SystemExit(1)
if rt.get("leader_heartbeat_ms") != 250 or rt.get("leader_lease_ms") != 900:
  print("runtime summary missing leader freshness config", rt, file=sys.stderr)
  raise SystemExit(1)
if rt.get("membership_epoch") != 12:
  print("runtime summary missing membership epoch", rt, file=sys.stderr)
  raise SystemExit(1)
if sorted(rt.get("member_node_ids") or []) != sorted(["${NODE_A}", "${NODE_B}", "${NODE_C}"]):
  print("runtime summary missing member set", rt, file=sys.stderr)
  raise SystemExit(1)
res = rt.get("result") or {}
if res.get("leader_node_id") != leader or res.get("committed_decision_sha256") != decision_sha:
  print("edge node runtime result mismatch", rt, file=sys.stderr)
  raise SystemExit(1)

rows = json.loads(r'''${NODES_JSON}''').get("nodes") or []
lookup = {row.get("node_id"): row for row in rows}
for node_id in ("${NODE_A}", "${NODE_B}", "${NODE_C}"):
  row = lookup.get(node_id) or {}
  rt = row.get("consensus_runtime") or {}
  if rt.get("node_id") != node_id:
    print("edge nodes list missing runtime summary", node_id, row, file=sys.stderr)
    raise SystemExit(1)

running_stop = json.loads(r'''${RUNNING_STOP_JSON}''')
if not (running_stop.get("runtime") or {}).get("running"):
  print("stop runtime never entered running state", running_stop, file=sys.stderr)
  raise SystemExit(1)

stop_resp = json.loads(r'''${STOP_RESP_JSON}''')
if not stop_resp.get("ok") or not stop_resp.get("stopped"):
  print("stop response wrong", stop_resp, file=sys.stderr)
  raise SystemExit(1)

stop_status = json.loads(r'''${STOP_STATUS_JSON}''')
stop_rt = stop_status.get("runtime") or {}
if stop_rt.get("running"):
  print("stop runtime still running", stop_status, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_edge_consensus_runtime_smoke OK"
