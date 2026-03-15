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

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_consensus_transport_smoke" \
  --tools none

agentd_smoke_wait_health "${DAEMON_URL}"

send_env() {
  local payload="$1"
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Content-Type: application/json" \
    -d "${payload}" \
    "${DAEMON_URL}/api/v1/edge/message"
}

hello_node() {
  local node_id="$1"
  local caps_sha="$2"
  local payload
  payload="$(python3 - <<PY
import json, uuid, time
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "NODE_HELLO",
  "from": "node:${node_id}",
  "to": "platform",
  "body": {
    "node_id": "${node_id}",
    "model": "consensus_stub",
    "fw_git_sha": "deadbeef",
    "caps_sha256": "${caps_sha}"
  }
}))
PY
)"
  send_env "${payload}" >/dev/null
}

NODE_A="node_cons_a"
NODE_B="node_cons_b"
NODE_C="node_cons_c"
CAPS_SHA_A="sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
CAPS_SHA_B="sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
CAPS_SHA_C="sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"

hello_node "${NODE_A}" "${CAPS_SHA_A}"
hello_node "${NODE_B}" "${CAPS_SHA_B}"
hello_node "${NODE_C}" "${CAPS_SHA_C}"

REQ_JSON="$(python3 - <<PY
import json, uuid, time
frame = {
  "schema": "edge_node_consensus_frame_v1",
  "frame_id": "frame_vote_request_a_1",
  "kind": "vote_request",
  "term": 3,
  "decision_sha256": "sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
  "candidate_node_id": "${NODE_A}",
  "from": {
    "cluster_id": "lab-consensus",
    "node_id": "${NODE_A}",
    "manifest_sha256": "${CAPS_SHA_A}",
    "trust_epochs": {
      "trust_roots_epoch": 4,
      "revocations_epoch": 2,
      "cert_roots_epoch": 5
    }
  }
}
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "CONSENSUS_FRAME",
  "from": "node:${NODE_A}",
  "to": "platform",
  "body": {
    "target_node_ids": ["${NODE_B}", "${NODE_C}"],
    "frame": frame
  }
}))
PY
)"

REQ_RESP="$(send_env "${REQ_JSON}")"
python3 - <<PY
import json, sys
obj = json.loads(r'''${REQ_RESP}''')
if not obj.get("ok"):
  print("consensus request not ok", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("forwarded_count") != 2:
  print("expected forwarded_count=2", obj, file=sys.stderr)
  raise SystemExit(1)
frame = obj.get("frame") or {}
if frame.get("kind") != "vote_request" or frame.get("term") != 3:
  print("unexpected returned frame", frame, file=sys.stderr)
  raise SystemExit(1)
PY

OUTBOX_B="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/outbox?node_id=${NODE_B}&cursor=0&limit=50")"
OUTBOX_C="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/outbox?node_id=${NODE_C}&cursor=0&limit=50")"

python3 - <<PY
import json, sys
for label, raw in (("B", r'''${OUTBOX_B}'''), ("C", r'''${OUTBOX_C}''')):
  obj = json.loads(raw)
  msgs = obj.get("messages") or []
  found = None
  for row in msgs:
    env = row.get("msg") or {}
    body = env.get("body") or {}
    frame = body.get("frame") or {}
    if env.get("type") == "CONSENSUS_FRAME" and frame.get("frame_id") == "frame_vote_request_a_1":
      found = (env, body, frame)
      break
  if found is None:
    print(f"missing relayed consensus frame in outbox {label}", obj, file=sys.stderr)
    raise SystemExit(1)
  env, body, frame = found
  if env.get("from") != "platform" or body.get("relay_from") != "${NODE_A}":
    print(f"unexpected relay envelope in outbox {label}", env, body, file=sys.stderr)
    raise SystemExit(1)
  if frame.get("kind") != "vote_request" or frame.get("candidate_node_id") != "${NODE_A}":
    print(f"unexpected frame in outbox {label}", frame, file=sys.stderr)
    raise SystemExit(1)
PY

GRANT_JSON="$(python3 - <<PY
import json, uuid, time
frame = {
  "schema": "edge_node_consensus_frame_v1",
  "frame_id": "frame_vote_grant_b_1",
  "kind": "vote_grant",
  "term": 3,
  "decision_sha256": "sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
  "candidate_node_id": "${NODE_A}",
  "granted": True,
  "from": {
    "cluster_id": "lab-consensus",
    "node_id": "${NODE_B}",
    "manifest_sha256": "${CAPS_SHA_B}",
    "trust_epochs": {
      "trust_roots_epoch": 4,
      "revocations_epoch": 2,
      "cert_roots_epoch": 5
    }
  }
}
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "CONSENSUS_FRAME",
  "from": "node:${NODE_B}",
  "to": "platform",
  "body": {
    "target_node_id": "${NODE_A}",
    "frame": frame
  }
}))
PY
)"

send_env "${GRANT_JSON}" >/dev/null

OUTBOX_A="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/outbox?node_id=${NODE_A}&cursor=0&limit=100")"
python3 - <<PY
import json, sys
obj = json.loads(r'''${OUTBOX_A}''')
msgs = obj.get("messages") or []
found = None
for row in msgs:
  env = row.get("msg") or {}
  body = env.get("body") or {}
  frame = body.get("frame") or {}
  if env.get("type") == "CONSENSUS_FRAME" and frame.get("frame_id") == "frame_vote_grant_b_1":
    found = (env, body, frame)
    break
if found is None:
  print("missing relayed vote_grant in node A outbox", obj, file=sys.stderr)
  raise SystemExit(1)
env, body, frame = found
if body.get("relay_from") != "${NODE_B}" or frame.get("kind") != "vote_grant" or frame.get("granted") is not True:
  print("unexpected vote_grant relay", env, body, frame, file=sys.stderr)
  raise SystemExit(1)
PY

COMMIT_JSON="$(python3 - <<PY
import json, uuid, time
frame = {
  "schema": "edge_node_consensus_frame_v1",
  "frame_id": "frame_leader_commit_a_1",
  "kind": "leader_commit",
  "term": 3,
  "decision_sha256": "sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
  "leader_node_id": "${NODE_A}",
  "from": {
    "cluster_id": "lab-consensus",
    "node_id": "${NODE_A}",
    "manifest_sha256": "${CAPS_SHA_A}",
    "trust_epochs": {
      "trust_roots_epoch": 4,
      "revocations_epoch": 2,
      "cert_roots_epoch": 5
    }
  },
  "vote_witnesses": [
    {
      "cluster_id": "lab-consensus",
      "node_id": "${NODE_A}",
      "manifest_sha256": "${CAPS_SHA_A}",
      "trust_epochs": {
        "trust_roots_epoch": 4,
        "revocations_epoch": 2,
        "cert_roots_epoch": 5
      }
    },
    {
      "cluster_id": "lab-consensus",
      "node_id": "${NODE_B}",
      "manifest_sha256": "${CAPS_SHA_B}",
      "trust_epochs": {
        "trust_roots_epoch": 4,
        "revocations_epoch": 2,
        "cert_roots_epoch": 5
      }
    }
  ]
}
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time()*1000),
  "type": "CONSENSUS_FRAME",
  "from": "node:${NODE_A}",
  "to": "platform",
  "body": {
    "target_node_ids": ["${NODE_B}", "${NODE_C}"],
    "frame": frame
  }
}))
PY
)"

send_env "${COMMIT_JSON}" >/dev/null

NODE_A_JSON="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/node?node_id=${NODE_A}")"
NODE_B_JSON="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/node?node_id=${NODE_B}")"
NODES_JSON="$(curl -fsS --noproxy "*" --max-time 10 \
  "${DAEMON_URL}/api/v1/edge/nodes?limit=10")"

python3 - <<PY
import json, sys
node_a = json.loads(r'''${NODE_A_JSON}''').get("node") or {}
node_b = json.loads(r'''${NODE_B_JSON}''').get("node") or {}
cons_a = node_a.get("consensus") or {}
cons_b = node_b.get("consensus") or {}
if cons_a.get("last_frame_kind") != "leader_commit":
  print("node A consensus summary missing leader_commit", cons_a, file=sys.stderr)
  raise SystemExit(1)
if cons_a.get("leader_node_id") != "${NODE_A}" or cons_a.get("vote_witness_count") != 2:
  print("node A consensus summary wrong", cons_a, file=sys.stderr)
  raise SystemExit(1)
targets = cons_a.get("target_node_ids") or []
if sorted(targets) != sorted(["${NODE_B}", "${NODE_C}"]):
  print("node A consensus targets wrong", cons_a, file=sys.stderr)
  raise SystemExit(1)
if cons_b.get("last_frame_kind") != "vote_grant" or cons_b.get("candidate_node_id") != "${NODE_A}":
  print("node B consensus summary wrong", cons_b, file=sys.stderr)
  raise SystemExit(1)
nodes = json.loads(r'''${NODES_JSON}''').get("nodes") or []
row_a = None
for row in nodes:
  if row.get("node_id") == "${NODE_A}":
    row_a = row
    break
if not row_a or (row_a.get("consensus") or {}).get("leader_node_id") != "${NODE_A}":
  print("node list missing consensus summary", nodes, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_edge_consensus_transport_smoke OK"
