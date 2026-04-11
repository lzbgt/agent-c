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
DAEMON_TOKEN="agentd-edge-consensus-membership-token"
TEST_DB="${LOG_DIR}/agentd_edge_consensus_membership_bundle_smoke_${PORT_DAEMON}.sqlite"
TEST_STATE="${LOG_DIR}/agentd_edge_consensus_membership_bundle_smoke_${PORT_DAEMON}.state"

rm -f "${TEST_DB}"
rm -rf "${TEST_STATE}"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

export AGENTD_AUTH_TOKEN="${DAEMON_TOKEN}"
export AGENTD_RUN_ATTEST_HMAC_KID="edge-consensus-membership-k0"
export AGENTD_RUN_ATTEST_HMAC_KEY="edge-consensus-membership-secret"

agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_consensus_membership_bundle_smoke" \
  --db-path "${TEST_DB}" \
  --state-dir "${TEST_STATE}" \
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

wait_runtime_done() {
  local node_id="$1"
  local status_json=""
  for _ in $(seq 1 220); do
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

register_target_node() {
  local node_id="$1"
  local caps_sha="$2"
  local hello_json
  hello_json="$(python3 - <<PY
import json, time, uuid
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time() * 1000),
  "type": "NODE_HELLO",
  "from": "node:${node_id}",
  "to": "platform",
  "body": {
    "node_id": "${node_id}",
    "model": "consensus_bundle_target",
    "fw_git_sha": "membership-bundle-f00d",
    "caps_sha256": "${caps_sha}",
  },
}))
PY
)"
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Authorization: Bearer ${DAEMON_TOKEN}" \
    -H 'Content-Type: application/json' \
    -d "${hello_json}" \
    "${DAEMON_URL}/api/v1/edge/message" >/dev/null

  local caps_rsp_json
  caps_rsp_json="$(python3 - <<PY
import json, time, uuid
manifest = {
  "spec_version": "um-acds/0.1",
  "manifest_version": "0.0.1",
  "caps_sha256": "${caps_sha}",
  "node": {"node_id": "${node_id}"},
  "runtime": {"agent_core": {"version": "0.0.0"}},
  "hardware": {"presence": {"radio.ble": "present"}},
  "tools": [{
    "name": "consensus.bundle.recv",
    "kind": "sensor",
    "description": "Consensus bundle receive smoke tool",
    "parameters_schema": {"type":"object","additionalProperties": False, "properties": {}},
    "timeout_ms": 500,
    "idempotent": True,
    "side_effect_level": "none",
    "hazards": []
  }],
  "safety": {},
  "tags": ["edge:recipient"]
}
print(json.dumps({
  "msg_id": str(uuid.uuid4()),
  "ts_utc_ms": int(time.time() * 1000),
  "type": "NODE_CAPS_RSP",
  "from": "node:${node_id}",
  "to": "platform",
  "body": {"node_id": "${node_id}", "manifest": manifest},
}))
PY
)"
  curl -fsS --noproxy "*" --max-time 10 \
    -H "Authorization: Bearer ${DAEMON_TOKEN}" \
    -H 'Content-Type: application/json' \
    -d "${caps_rsp_json}" \
    "${DAEMON_URL}/api/v1/edge/message" >/dev/null
}

CLUSTER_ID="lab-consensus-policy"
NODE_A="node_policy_cons_a"
NODE_B="node_policy_cons_b"
NODE_C="node_policy_cons_c"
TARGET_NODE="node_policy_target_1"
CAPS_SHA_A="sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
CAPS_SHA_B="sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
CAPS_SHA_C="sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
TARGET_CAPS_SHA="sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
DECISION_SHA="sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"

register_target_node "${TARGET_NODE}" "${TARGET_CAPS_SHA}"

CONFIG_JSON="$(curl_json POST "/api/v1/config/update" "$(cat <<JSON
{"edge_auth_trust_roots_epoch":2,"edge_auth_revocations_epoch":3,"edge_auth_cert_roots_epoch":4}
JSON
)")"

ROTATE_SEED_17_JSON="$(curl_json POST "/api/v1/edge/consensus/membership/rotate" "$(cat <<JSON
{"cluster_id":"${CLUSTER_ID}","mode":"replace","membership_epoch":17,"member_node_ids":["${NODE_A}"],"campaign_delay_ms":80,"campaign_retry_ms":200,"campaign_retry_max_ms":600,"campaign_retry_backoff_factor":2,"leader_heartbeat_ms":210,"leader_lease_ms":900,"lease_expiry_recampaign_delay_ms":300}
JSON
)")"

ROTATE_SEED_JSON="$(curl_json POST "/api/v1/edge/consensus/membership/rotate" "$(cat <<JSON
{"cluster_id":"${CLUSTER_ID}","mode":"replace","membership_epoch":18,"member_node_ids":["${NODE_A}","${NODE_B}"],"campaign_delay_ms":90,"campaign_retry_ms":250,"campaign_retry_max_ms":750,"campaign_retry_backoff_factor":2,"leader_heartbeat_ms":220,"leader_lease_ms":1000,"lease_expiry_recampaign_delay_ms":350}
JSON
)")"

agentd_smoke_stop
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_consensus_membership_bundle_smoke" \
  --db-path "${TEST_DB}" \
  --state-dir "${TEST_STATE}" \
  --tools none
agentd_smoke_wait_health "${DAEMON_URL}" "${DAEMON_TOKEN}"
register_target_node "${TARGET_NODE}" "${TARGET_CAPS_SHA}"

ROTATE_JSON="$(curl_json POST "/api/v1/edge/consensus/membership/rotate" "$(cat <<JSON
{"cluster_id":"${CLUSTER_ID}","mode":"replace","membership_epoch":19,"member_node_ids":["${NODE_A}","${NODE_B}","${NODE_C}"],"campaign_delay_ms":120,"campaign_retry_ms":300,"campaign_retry_max_ms":900,"campaign_retry_backoff_factor":2,"leader_heartbeat_ms":240,"leader_lease_ms":1100,"lease_expiry_recampaign_delay_ms":410}
JSON
)")"
GET_JSON="$(curl_json GET "/api/v1/edge/consensus/membership?cluster_id=${CLUSTER_ID}")"
SEND_JSON="$(curl_json POST "/api/v1/edge/consensus/membership/send" "$(cat <<JSON
{"cluster_id":"${CLUSTER_ID}","target_node_id":"${TARGET_NODE}"}
JSON
)")"
OUTBOX_JSON="$(curl_json GET "/api/v1/edge/outbox?node_id=${TARGET_NODE}&cursor=0&limit=50")"

START_A_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"start","node_id":"${NODE_A}","cluster_id":"${CLUSTER_ID}","manifest_sha256":"${CAPS_SHA_A}","decision_sha256":"${DECISION_SHA}","trust_roots_epoch":2,"revocations_epoch":3,"cert_roots_epoch":4,"deadline_ms":18000}
JSON
)")"
RUNNING_A_JSON="$(wait_runtime_running "${NODE_A}")"

sleep 1.2

START_B_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"start","node_id":"${NODE_B}","cluster_id":"${CLUSTER_ID}","manifest_sha256":"${CAPS_SHA_B}","trust_roots_epoch":2,"revocations_epoch":3,"cert_roots_epoch":4,"deadline_ms":18000}
JSON
)")"
START_C_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"start","node_id":"${NODE_C}","cluster_id":"${CLUSTER_ID}","manifest_sha256":"${CAPS_SHA_C}","trust_roots_epoch":2,"revocations_epoch":3,"cert_roots_epoch":4,"deadline_ms":18000}
JSON
)")"

STATUS_A_JSON="$(wait_runtime_done "${NODE_A}")"
STATUS_B_JSON="$(wait_runtime_done "${NODE_B}")"
STATUS_C_JSON="$(wait_runtime_done "${NODE_C}")"
STOP_DONE_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"stop","node_id":"${NODE_B}"}
JSON
)")"

python3 - <<PY
import json, sys

cluster_id = "${CLUSTER_ID}"
members = sorted(["${NODE_A}", "${NODE_B}", "${NODE_C}"])
previous_members = sorted(["${NODE_A}", "${NODE_B}"])
older_members = sorted(["${NODE_A}"])
decision_sha = "${DECISION_SHA}"

rotate_seed_17 = json.loads(r'''${ROTATE_SEED_17_JSON}''')
rotate_seed = json.loads(r'''${ROTATE_SEED_JSON}''')
rotate = json.loads(r'''${ROTATE_JSON}''')
get_obj = json.loads(r'''${GET_JSON}''')
send_obj = json.loads(r'''${SEND_JSON}''')
outbox = json.loads(r'''${OUTBOX_JSON}''')
config_obj = json.loads(r'''${CONFIG_JSON}''')
start_a = json.loads(r'''${START_A_JSON}''')
running_a = json.loads(r'''${RUNNING_A_JSON}''')
start_b = json.loads(r'''${START_B_JSON}''')
start_c = json.loads(r'''${START_C_JSON}''')
status_a = json.loads(r'''${STATUS_A_JSON}''')
status_b = json.loads(r'''${STATUS_B_JSON}''')
status_c = json.loads(r'''${STATUS_C_JSON}''')
stop_done = json.loads(r'''${STOP_DONE_JSON}''')

if not config_obj.get("ok"):
    print("config", config_obj, file=sys.stderr)
    raise SystemExit(1)

for label, obj in (("rotate_seed_17", rotate_seed_17), ("rotate_seed", rotate_seed), ("rotate", rotate), ("get", get_obj), ("send", send_obj), ("start_a", start_a), ("start_b", start_b), ("start_c", start_c)):
    if not obj.get("ok"):
        print(label, obj, file=sys.stderr)
        raise SystemExit(1)
    if label.startswith("start_") and obj.get("startup_confirmed") is not True:
        print(label, "missing startup_confirmed=true", obj, file=sys.stderr)
        raise SystemExit(1)

bundle = get_obj.get("membership") or {}
if bundle.get("schema") != "edge_consensus_membership_v1":
    print("wrong membership schema", bundle, file=sys.stderr)
    raise SystemExit(1)
if bundle.get("cluster_id") != cluster_id or bundle.get("membership_epoch") != 19:
    print("wrong membership identity", bundle, file=sys.stderr)
    raise SystemExit(1)
if sorted(bundle.get("member_node_ids") or []) != members:
    print("wrong membership members", bundle, file=sys.stderr)
    raise SystemExit(1)
if bundle.get("previous_membership_epoch") != 18 or sorted(bundle.get("previous_member_node_ids") or []) != previous_members:
    print("wrong membership lineage", bundle, file=sys.stderr)
    raise SystemExit(1)
lineage = bundle.get("membership_lineage") or []
if len(lineage) < 2 or lineage[0].get("membership_epoch") != 18 or sorted(lineage[0].get("member_node_ids") or []) != previous_members:
    print("wrong immediate membership lineage history", bundle, file=sys.stderr)
    raise SystemExit(1)
if lineage[1].get("membership_epoch") != 17 or sorted(lineage[1].get("member_node_ids") or []) != older_members:
    print("wrong older membership lineage history", bundle, file=sys.stderr)
    raise SystemExit(1)
if bundle.get("campaign_delay_ms") != 120:
    print("wrong campaign delay", bundle, file=sys.stderr)
    raise SystemExit(1)
if bundle.get("campaign_retry_ms") != 300 or bundle.get("campaign_retry_max_ms") != 900:
    print("wrong retry bounds", bundle, file=sys.stderr)
    raise SystemExit(1)
if bundle.get("campaign_retry_backoff_factor") != 2:
    print("wrong retry backoff factor", bundle, file=sys.stderr)
    raise SystemExit(1)
if bundle.get("leader_heartbeat_ms") != 240 or bundle.get("leader_lease_ms") != 1100:
    print("wrong leader freshness policy", bundle, file=sys.stderr)
    raise SystemExit(1)
if bundle.get("lease_expiry_recampaign_delay_ms") != 410:
    print("wrong lease expiry recampaign policy", bundle, file=sys.stderr)
    raise SystemExit(1)
att = bundle.get("attest") or {}
if att.get("schema") != "edge_consensus_membership_attest_v1" or att.get("kid") != "edge-consensus-membership-k0":
    print("missing membership attest", bundle, file=sys.stderr)
    raise SystemExit(1)
if att.get("previous_membership_epoch") != 18:
    print("wrong membership attest lineage", bundle, file=sys.stderr)
    raise SystemExit(1)
if rotate.get("previous_membership_epoch") != 18 or rotate.get("previous_member_count") != 2:
    print("rotate lineage missing", rotate, file=sys.stderr)
    raise SystemExit(1)

msgs = outbox.get("messages") or []
membership_msgs = [m for m in msgs if (m.get("msg") or {}).get("type") == "PLATFORM_CONSENSUS_MEMBERSHIP_BUNDLE"]
if not membership_msgs:
    print("missing membership outbox message", outbox, file=sys.stderr)
    raise SystemExit(1)
payload = (membership_msgs[-1].get("msg") or {}).get("body") or {}
delivered = payload.get("membership") or {}
if delivered.get("cluster_id") != cluster_id or delivered.get("membership_epoch") != 19:
    print("wrong delivered membership bundle", delivered, file=sys.stderr)
    raise SystemExit(1)
if delivered.get("previous_membership_epoch") != 18 or sorted(delivered.get("previous_member_node_ids") or []) != previous_members:
    print("wrong delivered membership lineage", delivered, file=sys.stderr)
    raise SystemExit(1)
delivered_lineage = delivered.get("membership_lineage") or []
if len(delivered_lineage) < 2 or delivered_lineage[0].get("membership_epoch") != 18 or delivered_lineage[1].get("membership_epoch") != 17:
    print("wrong delivered membership lineage history", delivered, file=sys.stderr)
    raise SystemExit(1)
if delivered.get("campaign_retry_ms") != 300 or delivered.get("campaign_retry_max_ms") != 900:
    print("wrong delivered retry bounds", delivered, file=sys.stderr)
    raise SystemExit(1)
if delivered.get("campaign_retry_backoff_factor") != 2:
    print("wrong delivered backoff factor", delivered, file=sys.stderr)
    raise SystemExit(1)
if delivered.get("leader_heartbeat_ms") != 240 or delivered.get("leader_lease_ms") != 1100:
    print("wrong delivered leader freshness policy", delivered, file=sys.stderr)
    raise SystemExit(1)
if delivered.get("lease_expiry_recampaign_delay_ms") != 410:
    print("wrong delivered lease expiry recampaign policy", delivered, file=sys.stderr)
    raise SystemExit(1)

rt_a = start_a.get("runtime") or {}
if sorted(rt_a.get("member_node_ids") or []) != members:
    print("runtime A did not default member set from policy", rt_a, file=sys.stderr)
    raise SystemExit(1)
if rt_a.get("runtime_kind") != "builtin":
    print("runtime A did not use builtin runtime", rt_a, file=sys.stderr)
    raise SystemExit(1)
if sorted(rt_a.get("peer_node_ids") or []) != sorted(["${NODE_B}", "${NODE_C}"]):
    print("runtime A did not default peer set from policy", rt_a, file=sys.stderr)
    raise SystemExit(1)
if rt_a.get("membership_epoch") != 19 or rt_a.get("campaign_delay_ms") != 120 or rt_a.get("campaign_retry_ms") != 300:
    print("runtime A missing policy defaults", rt_a, file=sys.stderr)
    raise SystemExit(1)
if rt_a.get("campaign_retry_max_ms") != 900 or rt_a.get("campaign_retry_backoff_factor") != 2:
    print("runtime A missing retry backoff defaults", rt_a, file=sys.stderr)
    raise SystemExit(1)
if rt_a.get("leader_heartbeat_ms") != 240 or rt_a.get("leader_lease_ms") != 1100:
    print("runtime A missing leader freshness defaults", rt_a, file=sys.stderr)
    raise SystemExit(1)
if rt_a.get("lease_expiry_recampaign_delay_ms") != 410:
    print("runtime A missing lease expiry recampaign default", rt_a, file=sys.stderr)
    raise SystemExit(1)
trust_epochs = rt_a.get("trust_epochs") or {}
if trust_epochs.get("trust_roots_epoch") != 2 or trust_epochs.get("revocations_epoch") != 3 or trust_epochs.get("cert_roots_epoch") != 4:
    print("runtime A missing explicit trust epochs", rt_a, file=sys.stderr)
    raise SystemExit(1)
if (start_a.get("cluster_policy") or {}).get("cluster_id") != cluster_id:
    print("start response missing cluster policy", start_a, file=sys.stderr)
    raise SystemExit(1)

for label, obj in (("running_a", running_a), ("status_a", status_a), ("status_b", status_b), ("status_c", status_c)):
    rt = obj.get("runtime") or {}
    if sorted(rt.get("member_node_ids") or []) != members:
        print(label, "runtime member set mismatch", obj, file=sys.stderr)
        raise SystemExit(1)
    if rt.get("membership_epoch") != 19:
        print(label, "runtime membership epoch mismatch", obj, file=sys.stderr)
        raise SystemExit(1)
    if rt.get("campaign_retry_ms") != 300 or rt.get("campaign_retry_max_ms") != 900:
        print(label, "runtime retry bounds mismatch", obj, file=sys.stderr)
        raise SystemExit(1)
    if rt.get("campaign_retry_backoff_factor") != 2:
        print(label, "runtime retry backoff mismatch", obj, file=sys.stderr)
        raise SystemExit(1)
    if rt.get("leader_heartbeat_ms") != 240 or rt.get("leader_lease_ms") != 1100:
        print(label, "runtime leader freshness mismatch", obj, file=sys.stderr)
        raise SystemExit(1)
    if rt.get("lease_expiry_recampaign_delay_ms") != 410:
        print(label, "runtime lease expiry recampaign mismatch", obj, file=sys.stderr)
        raise SystemExit(1)
    epochs = rt.get("trust_epochs") or {}
    if epochs.get("trust_roots_epoch") != 2 or epochs.get("revocations_epoch") != 3 or epochs.get("cert_roots_epoch") != 4:
        print(label, "runtime trust epoch mismatch", obj, file=sys.stderr)
        raise SystemExit(1)

res_a = (status_a.get("runtime") or {}).get("result") or {}
res_b = (status_b.get("runtime") or {}).get("result") or {}
res_c = (status_c.get("runtime") or {}).get("result") or {}
for label, res in (("A", res_a), ("B", res_b), ("C", res_c)):
    if not res.get("ok"):
        print("runtime result not ok", label, res, file=sys.stderr)
        raise SystemExit(1)
    if res.get("committed_decision_sha256") != decision_sha:
        print("wrong decision sha", label, res, file=sys.stderr)
        raise SystemExit(1)

loop_status = res_a.get("status") or {}
if loop_status.get("campaign_attempts", 0) < 2:
    print("runtime A did not retry after late peers", loop_status, file=sys.stderr)
    raise SystemExit(1)
if loop_status.get("campaign_retry_max_ms") != 900 or loop_status.get("campaign_retry_backoff_factor") != 2:
    print("loop status missing retry backoff policy", loop_status, file=sys.stderr)
    raise SystemExit(1)
if loop_status.get("leader_heartbeat_ms") != 240 or loop_status.get("leader_lease_ms") != 1100:
    print("loop status missing leader freshness policy", loop_status, file=sys.stderr)
    raise SystemExit(1)
if loop_status.get("lease_expiry_recampaign_delay_ms") != 410:
    print("loop status missing lease expiry recampaign policy", loop_status, file=sys.stderr)
    raise SystemExit(1)
loop_self_epochs = ((loop_status.get("self") or {}).get("trust_epochs") or {})
if loop_self_epochs.get("trust_roots_epoch") != 2 or loop_self_epochs.get("revocations_epoch") != 3 or loop_self_epochs.get("cert_roots_epoch") != 4:
    print("loop status missing explicit trust epochs", loop_status, file=sys.stderr)
    raise SystemExit(1)

status_policy = status_a.get("cluster_policy") or {}
if status_policy.get("cluster_id") != cluster_id:
    print("runtime status missing cluster policy", status_a, file=sys.stderr)
    raise SystemExit(1)
if sorted(status_policy.get("member_node_ids") or []) != members:
    print("runtime status cluster policy mismatch", status_policy, file=sys.stderr)
    raise SystemExit(1)
if status_policy.get("previous_membership_epoch") != 18 or sorted(status_policy.get("previous_member_node_ids") or []) != previous_members:
    print("runtime status cluster policy lineage mismatch", status_policy, file=sys.stderr)
    raise SystemExit(1)
status_lineage = status_policy.get("membership_lineage") or []
if len(status_lineage) < 2 or status_lineage[0].get("membership_epoch") != 18 or status_lineage[1].get("membership_epoch") != 17:
    print("runtime status cluster policy lineage history mismatch", status_policy, file=sys.stderr)
    raise SystemExit(1)
if status_policy.get("campaign_retry_ms") != 300 or status_policy.get("campaign_retry_max_ms") != 900:
    print("runtime status missing retry bounds in cluster policy", status_policy, file=sys.stderr)
    raise SystemExit(1)
if status_policy.get("campaign_retry_backoff_factor") != 2:
    print("runtime status missing retry backoff factor in cluster policy", status_policy, file=sys.stderr)
    raise SystemExit(1)
if status_policy.get("leader_heartbeat_ms") != 240 or status_policy.get("leader_lease_ms") != 1100:
    print("runtime status missing leader freshness policy in cluster policy", status_policy, file=sys.stderr)
    raise SystemExit(1)
if status_policy.get("lease_expiry_recampaign_delay_ms") != 410:
    print("runtime status missing lease expiry recampaign policy in cluster policy", status_policy, file=sys.stderr)
    raise SystemExit(1)

if not stop_done.get("ok") or stop_done.get("stopped") is not False or stop_done.get("reason") != "not_running":
    print("stop completed runtime wrong", stop_done, file=sys.stderr)
    raise SystemExit(1)
stop_rt = stop_done.get("runtime") or {}
stop_res = stop_rt.get("result") or {}
if stop_rt.get("running"):
    print("stop completed runtime unexpectedly running", stop_done, file=sys.stderr)
    raise SystemExit(1)
if stop_rt.get("runtime_kind") != "builtin" or not stop_res.get("ok"):
    print("stop completed runtime lost final snapshot", stop_done, file=sys.stderr)
    raise SystemExit(1)

print("ok")
PY
