#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"

AGENTD_BIN="${1:-}"
NODE_TOOL_BIN="${2:-}"
if [[ -z "${AGENTD_BIN}" || -z "${NODE_TOOL_BIN}" ]]; then
  echo "usage: $0 <agentd_bin> <agentd_edge_consensus_node_bin>" >&2
  exit 2
fi

LOG_DIR="$(agentd_smoke_log_dir)"
mkdir -p "${LOG_DIR}"

PORT_DAEMON="$(agentd_smoke_pick_port)"
HOST="127.0.0.1"

cleanup() {
  for pid_var in NODE_A_PID NODE_B_PID NODE_C_PID; do
    local pid="${!pid_var:-}"
    if [[ -n "${pid}" ]]; then
      kill -TERM "${pid}" >/dev/null 2>&1 || true
      wait "${pid}" >/dev/null 2>&1 || true
    fi
  done
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_consensus_autonomous_smoke" \
  --tools none
agentd_smoke_wait_health "${DAEMON_URL}"

NODE_A="node_auto_cons_a"
NODE_B="node_auto_cons_b"
NODE_C="node_auto_cons_c"
CLUSTER_ID="lab-consensus-auto"
CAPS_SHA_A="sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
CAPS_SHA_B="sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
CAPS_SHA_C="sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
DECISION_SHA="sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"

NODE_A_LOG="${LOG_DIR}/agentd_edge_consensus_autonomous_node_a.log"
NODE_B_LOG="${LOG_DIR}/agentd_edge_consensus_autonomous_node_b.log"
NODE_C_LOG="${LOG_DIR}/agentd_edge_consensus_autonomous_node_c.log"

"${NODE_TOOL_BIN}" \
  --daemon-url "${DAEMON_URL}" \
  --node-id "${NODE_B}" \
  --cluster-id "${CLUSTER_ID}" \
  --manifest-sha256 "${CAPS_SHA_B}" \
  --peer-node-id "${NODE_A}" \
  --peer-node-id "${NODE_C}" \
  --trust-roots-epoch 4 \
  --revocations-epoch 2 \
  --cert-roots-epoch 5 \
  --deadline-ms 12000 \
  >"${NODE_B_LOG}" 2>&1 &
NODE_B_PID=$!

"${NODE_TOOL_BIN}" \
  --daemon-url "${DAEMON_URL}" \
  --node-id "${NODE_C}" \
  --cluster-id "${CLUSTER_ID}" \
  --manifest-sha256 "${CAPS_SHA_C}" \
  --peer-node-id "${NODE_A}" \
  --peer-node-id "${NODE_B}" \
  --trust-roots-epoch 4 \
  --revocations-epoch 2 \
  --cert-roots-epoch 5 \
  --deadline-ms 12000 \
  >"${NODE_C_LOG}" 2>&1 &
NODE_C_PID=$!

sleep 0.3

"${NODE_TOOL_BIN}" \
  --daemon-url "${DAEMON_URL}" \
  --node-id "${NODE_A}" \
  --cluster-id "${CLUSTER_ID}" \
  --manifest-sha256 "${CAPS_SHA_A}" \
  --peer-node-id "${NODE_B}" \
  --peer-node-id "${NODE_C}" \
  --trust-roots-epoch 4 \
  --revocations-epoch 2 \
  --cert-roots-epoch 5 \
  --decision-sha256 "${DECISION_SHA}" \
  --campaign-delay-ms 300 \
  --deadline-ms 12000 \
  >"${NODE_A_LOG}" 2>&1 &
NODE_A_PID=$!

wait "${NODE_A_PID}"
wait "${NODE_B_PID}"
wait "${NODE_C_PID}"

NODE_A_JSON="$(tail -n 1 "${NODE_A_LOG}")"
NODE_B_JSON="$(tail -n 1 "${NODE_B_LOG}")"
NODE_C_JSON="$(tail -n 1 "${NODE_C_LOG}")"
NODE_REG_A_JSON="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/node?node_id=${NODE_A}")"
NODE_REG_B_JSON="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/node?node_id=${NODE_B}")"
NODE_REG_C_JSON="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/node?node_id=${NODE_C}")"
NODES_JSON="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/nodes?limit=10")"

python3 - <<PY
import json, sys

decision_sha = "${DECISION_SHA}"
member_ids = {"${NODE_A}", "${NODE_B}", "${NODE_C}"}

results = {}
for label, raw in (("A", r'''${NODE_A_JSON}'''), ("B", r'''${NODE_B_JSON}'''), ("C", r'''${NODE_C_JSON}''')):
  obj = json.loads(raw)
  results[label] = obj
  if not obj.get("ok"):
    print(f"node {label} did not exit cleanly", obj, file=sys.stderr)
    raise SystemExit(1)
  if obj.get("committed_decision_sha256") != decision_sha:
    print(f"node {label} wrong committed decision", obj, file=sys.stderr)
    raise SystemExit(1)

leader = results["A"].get("leader_node_id")
if leader not in member_ids:
  print("invalid elected leader", results, file=sys.stderr)
  raise SystemExit(1)

for label, obj in results.items():
  if obj.get("leader_node_id") != leader:
    print(f"node {label} wrong leader", obj, file=sys.stderr)
    raise SystemExit(1)

node_a = (json.loads(r'''${NODE_REG_A_JSON}''').get("node") or {}).get("consensus") or {}
node_b = (json.loads(r'''${NODE_REG_B_JSON}''').get("node") or {}).get("consensus") or {}
node_c = (json.loads(r'''${NODE_REG_C_JSON}''').get("node") or {}).get("consensus") or {}
registry = {"${NODE_A}": node_a, "${NODE_B}": node_b, "${NODE_C}": node_c}

leader_row = registry.get(leader) or {}
if leader_row.get("last_frame_kind") != "leader_commit" or leader_row.get("leader_node_id") != leader:
  print("leader registry consensus summary wrong", leader_row, file=sys.stderr)
  raise SystemExit(1)
if leader_row.get("decision_sha256") != decision_sha or leader_row.get("vote_witness_count", 0) < 2:
  print("leader registry summary missing quorum evidence", leader_row, file=sys.stderr)
  raise SystemExit(1)

for node_id, row in registry.items():
  if node_id == leader:
    continue
  if row.get("last_frame_kind") != "vote_grant":
    print(f"node {node_id} registry summary missing vote_grant", row, file=sys.stderr)
    raise SystemExit(1)
  if row.get("candidate_node_id") != leader:
    print(f"node {node_id} registry summary wrong candidate", row, file=sys.stderr)
    raise SystemExit(1)
  if row.get("current_term", 0) < 1:
    print(f"node {node_id} registry summary wrong term", row, file=sys.stderr)
    raise SystemExit(1)

rows = json.loads(r'''${NODES_JSON}''').get("nodes") or []
seen = {row.get("node_id"): row.get("consensus") or {} for row in rows}
for node_id in ("${NODE_A}", "${NODE_B}", "${NODE_C}"):
  if node_id not in seen:
    print("node list missing node", node_id, rows, file=sys.stderr)
    raise SystemExit(1)
if seen[leader].get("leader_node_id") != leader:
  print("node list missing leader consensus summary", seen, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_edge_consensus_autonomous_smoke OK"
