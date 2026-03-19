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
DAEMON_TOKEN="agentd-edge-consensus-runtime-token"
TEST_DB="${LOG_DIR}/agentd_edge_consensus_runtime.sqlite"
TEST_STATE="${LOG_DIR}/agentd_edge_consensus_runtime.state"

rm -f "${TEST_DB}"
rm -rf "${TEST_STATE}"

cleanup() {
  agentd_smoke_stop
}
trap cleanup EXIT

AGENTD_AUTH_TOKEN="${DAEMON_TOKEN}" \
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_consensus_runtime_smoke" \
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

curl_json_status() {
  local method="$1"
  local path="$2"
  local body="${3:-}"
  local tmp_body
  tmp_body="$(mktemp)"
  local status
  if [[ -n "${body}" ]]; then
    status="$(curl -sS --noproxy "*" --max-time 15 \
      -o "${tmp_body}" \
      -w '%{http_code}' \
      -X "${method}" \
      -H "Authorization: Bearer ${DAEMON_TOKEN}" \
      -H 'Content-Type: application/json' \
      -d "${body}" \
      "${DAEMON_URL}${path}")"
  else
    status="$(curl -sS --noproxy "*" --max-time 15 \
      -o "${tmp_body}" \
      -w '%{http_code}' \
      -X "${method}" \
      -H "Authorization: Bearer ${DAEMON_TOKEN}" \
      "${DAEMON_URL}${path}")"
  fi
  printf '%s\n' "${status}"
  cat "${tmp_body}"
  rm -f "${tmp_body}"
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

wait_runtime_running_persisted() {
  local node_id="$1"
  local status_json=""
  for _ in $(seq 1 120); do
    status_json="$(curl_json GET "/api/v1/edge/node/consensus_runtime?node_id=${node_id}")"
    if python3 - <<PY
import json
obj = json.loads(r'''${status_json}''')
rt = obj.get("runtime")
if not isinstance(rt, dict) or not rt.get("running") or rt.get("status_source") != "persisted":
  raise SystemExit(1)
PY
    then
      echo "${status_json}"
      return 0
    fi
    sleep 0.1
  done
  echo "timed out waiting for runtime ${node_id} to recover as persisted+running" >&2
  echo "${status_json}" >&2
  return 1
}

wait_runtime_stopped_with_signal() {
  local node_id="$1"
  local exit_signal="$2"
  local status_json=""
  for _ in $(seq 1 120); do
    status_json="$(curl_json GET "/api/v1/edge/node/consensus_runtime?node_id=${node_id}")"
    if python3 - <<PY
import json
obj = json.loads(r'''${status_json}''')
rt = obj.get("runtime")
if not isinstance(rt, dict) or rt.get("running"):
  raise SystemExit(1)
if int(rt.get("exit_signal") or 0) != int(${exit_signal}):
  raise SystemExit(1)
PY
    then
      echo "${status_json}"
      return 0
    fi
    sleep 0.1
  done
  echo "timed out waiting for runtime ${node_id} to stop with exit_signal=${exit_signal}" >&2
  echo "${status_json}" >&2
  return 1
}

wait_runtime_stopped() {
  local node_id="$1"
  local status_json=""
  for _ in $(seq 1 120); do
    status_json="$(curl_json GET "/api/v1/edge/node/consensus_runtime?node_id=${node_id}")"
    if python3 - <<PY
import json
obj = json.loads(r'''${status_json}''')
rt = obj.get("runtime")
if not isinstance(rt, dict) or rt.get("running"):
  raise SystemExit(1)
PY
    then
      echo "${status_json}"
      return 0
    fi
    sleep 0.1
  done
  echo "timed out waiting for runtime ${node_id} to stop" >&2
  echo "${status_json}" >&2
  return 1
}

wait_runtime_stopped_persisted() {
  local node_id="$1"
  local status_json=""
  for _ in $(seq 1 120); do
    status_json="$(curl_json GET "/api/v1/edge/node/consensus_runtime?node_id=${node_id}")"
    if python3 - <<PY
import json
obj = json.loads(r'''${status_json}''')
rt = obj.get("runtime")
if not isinstance(rt, dict) or rt.get("running") or rt.get("status_source") != "persisted":
  raise SystemExit(1)
PY
    then
      echo "${status_json}"
      return 0
    fi
    sleep 0.1
  done
  echo "timed out waiting for runtime ${node_id} to stop as persisted" >&2
  echo "${status_json}" >&2
  return 1
}

STOP_NODE="node_runtime_cons_stop"
STOP_CLUSTER="lab-consensus-managed-stop"
STOP_SHA="sha256:5555555555555555555555555555555555555555555555555555555555555555"
STALE_NODE="node_runtime_cons_stale"
STALE_CLUSTER="lab-consensus-managed-stale"
STALE_SHA="sha256:5858585858585858585858585858585858585858585858585858585858585858"
BUILTIN_LOCAL_NODE="node_runtime_cons_builtin_local"
BUILTIN_LOCAL_CLUSTER="lab-consensus-managed-builtin-local"
BUILTIN_LOCAL_SHA="sha256:5959595959595959595959595959595959595959595959595959595959595959"
DRIFT_NODE="node_runtime_cons_policy_drift"
DRIFT_CLUSTER="lab-consensus-policy-drift"
DRIFT_MEMBER_A="${DRIFT_NODE}"
DRIFT_MEMBER_B="node_runtime_cons_policy_peer_b"
DRIFT_MEMBER_C="node_runtime_cons_policy_peer_c"
DRIFT_MEMBER_D="node_runtime_cons_policy_peer_d"
DRIFT_SHA="sha256:5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a"
TRUST_NODE="node_runtime_cons_trust_drift"
TRUST_CLUSTER="lab-consensus-trust-drift"
TRUST_SHA="sha256:5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b5b"

START_STOP_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"start","node_id":"${STOP_NODE}","cluster_id":"${STOP_CLUSTER}","manifest_sha256":"${STOP_SHA}","deadline_ms":30000,"poll_interval_ms":100}
JSON
)")"
RUNNING_STOP_JSON="$(wait_runtime_running "${STOP_NODE}")"
STOP_RESP_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"stop","node_id":"${STOP_NODE}"}
JSON
)")"
STOP_STATUS_JSON="$(wait_runtime_stopped "${STOP_NODE}")"
BUILTIN_LOCAL_START_RAW="$(curl_json_status POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"start","node_id":"${BUILTIN_LOCAL_NODE}","cluster_id":"${BUILTIN_LOCAL_CLUSTER}","manifest_sha256":"${BUILTIN_LOCAL_SHA}","daemon_url":"not-a-url","auth_token":"unused-local-token","lease_expiry_recampaign_delay_ms":333,"deadline_ms":30000,"poll_interval_ms":100}
JSON
)")"
BUILTIN_LOCAL_START_STATUS="$(printf '%s\n' "${BUILTIN_LOCAL_START_RAW}" | sed -n '1p')"
BUILTIN_LOCAL_START_JSON="$(printf '%s\n' "${BUILTIN_LOCAL_START_RAW}" | sed '1d')"
BUILTIN_LOCAL_RUNNING_JSON="$(wait_runtime_running "${BUILTIN_LOCAL_NODE}")"
BUILTIN_LOCAL_AGAIN_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"start","node_id":"${BUILTIN_LOCAL_NODE}","cluster_id":"${BUILTIN_LOCAL_CLUSTER}","manifest_sha256":"${BUILTIN_LOCAL_SHA}","daemon_url":"https://ignored.example.invalid","auth_token":"different-unused-token","lease_expiry_recampaign_delay_ms":333,"deadline_ms":30000,"poll_interval_ms":100}
JSON
)")"
BUILTIN_LOCAL_STOP_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"stop","node_id":"${BUILTIN_LOCAL_NODE}"}
JSON
)")"

python3 - <<PY
import json, sys

start_stop = json.loads(r'''${START_STOP_JSON}''')
if not start_stop.get("ok") or not isinstance(start_stop.get("runtime"), dict):
  print("start_stop wrong", start_stop, file=sys.stderr)
  raise SystemExit(1)
if start_stop.get("startup_confirmed") is not True:
  print("start_stop missing startup_confirmed=true", start_stop, file=sys.stderr)
  raise SystemExit(1)
start_rt = start_stop.get("runtime") or {}
if start_rt.get("runtime_kind") != "builtin":
  print("start_stop runtime kind not builtin", start_stop, file=sys.stderr)
  raise SystemExit(1)
if not start_stop.get("builtin_available") or start_stop.get("default_runtime_kind") != "builtin":
  print("start_stop missing builtin metadata", start_stop, file=sys.stderr)
  raise SystemExit(1)
if start_stop.get("default_runtime_kind_source") != "auto":
  print("start_stop wrong default runtime source", start_stop, file=sys.stderr)
  raise SystemExit(1)
if start_stop.get("external_available"):
  print("start_stop unexpectedly reported external available", start_stop, file=sys.stderr)
  raise SystemExit(1)
if start_stop.get("external_unavailable_reason") != "edge_consensus_node_tool_path not configured":
  print("start_stop wrong external unavailable reason", start_stop, file=sys.stderr)
  raise SystemExit(1)

running_stop = json.loads(r'''${RUNNING_STOP_JSON}''')
if not (running_stop.get("runtime") or {}).get("running"):
  print("stop runtime never entered running state", running_stop, file=sys.stderr)
  raise SystemExit(1)
running_stop_live = ((running_stop.get("runtime") or {}).get("live_status") or {})
if (running_stop_live.get("self") or {}).get("node_id") != "${STOP_NODE}":
  print("stop runtime missing live_status self node", running_stop, file=sys.stderr)
  raise SystemExit(1)
if running_stop_live.get("cluster_size") != 1:
  print("stop runtime missing live_status cluster size", running_stop, file=sys.stderr)
  raise SystemExit(1)

stop_resp = json.loads(r'''${STOP_RESP_JSON}''')
if not stop_resp.get("ok") or not stop_resp.get("stopped"):
  print("stop response wrong", stop_resp, file=sys.stderr)
  raise SystemExit(1)

builtin_local_start = json.loads(r'''${BUILTIN_LOCAL_START_JSON}''')
builtin_local_running = json.loads(r'''${BUILTIN_LOCAL_RUNNING_JSON}''')
builtin_local_again = json.loads(r'''${BUILTIN_LOCAL_AGAIN_JSON}''')
builtin_local_stop = json.loads(r'''${BUILTIN_LOCAL_STOP_JSON}''')
if "${BUILTIN_LOCAL_START_STATUS}" != "200":
  print("builtin local start returned wrong status", "${BUILTIN_LOCAL_START_STATUS}", builtin_local_start, file=sys.stderr)
  raise SystemExit(1)
if builtin_local_start.get("startup_confirmed") is not True:
  print("builtin local start missing startup_confirmed=true", builtin_local_start, file=sys.stderr)
  raise SystemExit(1)
builtin_local_start_rt = builtin_local_start.get("runtime") or {}
if builtin_local_start_rt.get("runtime_kind") != "builtin":
  print("builtin local start missing runtime snapshot", builtin_local_start, file=sys.stderr)
  raise SystemExit(1)
if builtin_local_start_rt.get("daemon_url") != "@local":
  print("builtin local start did not normalize transport to @local", builtin_local_start, file=sys.stderr)
  raise SystemExit(1)
if builtin_local_start_rt.get("lease_expiry_recampaign_delay_ms") != 333:
  print("builtin local start missing lease-expiry recampaign override", builtin_local_start, file=sys.stderr)
  raise SystemExit(1)
if not (builtin_local_running.get("runtime") or {}).get("running"):
  print("builtin local runtime never entered running state", builtin_local_running, file=sys.stderr)
  raise SystemExit(1)
if not builtin_local_again.get("ok") or builtin_local_again.get("already_running") is not True:
  print("builtin local restart did not stay idempotent across transport-only drift", builtin_local_again, file=sys.stderr)
  raise SystemExit(1)
if (builtin_local_again.get("runtime") or {}).get("daemon_url") != "@local":
  print("builtin local restart lost normalized transport marker", builtin_local_again, file=sys.stderr)
  raise SystemExit(1)
if not builtin_local_stop.get("ok") or not builtin_local_stop.get("stopped"):
  print("builtin local stop failed", builtin_local_stop, file=sys.stderr)
  raise SystemExit(1)

stop_status = json.loads(r'''${STOP_STATUS_JSON}''')
stop_rt = stop_status.get("runtime") or {}
if stop_rt.get("running"):
  print("stop runtime still running", stop_status, file=sys.stderr)
  raise SystemExit(1)
if stop_rt.get("runtime_kind") != "builtin":
  print("stop runtime kind drifted", stop_status, file=sys.stderr)
  raise SystemExit(1)
if not stop_status.get("builtin_available") or stop_status.get("default_runtime_kind") != "builtin":
  print("stop status missing builtin metadata", stop_status, file=sys.stderr)
  raise SystemExit(1)
if stop_status.get("default_runtime_kind_source") != "auto":
  print("stop status wrong default runtime source", stop_status, file=sys.stderr)
  raise SystemExit(1)
if stop_status.get("external_available"):
  print("stop status unexpectedly reported external available", stop_status, file=sys.stderr)
  raise SystemExit(1)
PY

agentd_smoke_stop
AGENTD_AUTH_TOKEN="${DAEMON_TOKEN}" \
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_consensus_runtime_smoke_stop_restart" \
  --db-path "${TEST_DB}" \
  --state-dir "${TEST_STATE}" \
  --tools none
DAEMON_URL="http://${HOST}:${PORT_DAEMON}"
agentd_smoke_wait_health "${DAEMON_URL}" "${DAEMON_TOKEN}"

STOP_RESTART_STATUS_JSON="$(wait_runtime_stopped_persisted "${STOP_NODE}")"
python3 - <<PY
import sqlite3
conn = sqlite3.connect(r'''${TEST_DB}''')
try:
    conn.execute("DELETE FROM edge_nodes WHERE node_id = ?", (r'''${STOP_NODE}''',))
    conn.commit()
finally:
    conn.close()
PY
STOP_RESTART_NODE_JSON="$(curl_json GET "/api/v1/edge/node?node_id=${STOP_NODE}")"
STOP_RESTART_NODES_JSON="$(curl_json GET "/api/v1/edge/nodes?limit=200")"

START_STALE_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"start","node_id":"${STALE_NODE}","cluster_id":"${STALE_CLUSTER}","manifest_sha256":"${STALE_SHA}","deadline_ms":30000,"poll_interval_ms":100}
JSON
)")"
RUNNING_STALE_JSON="$(wait_runtime_running "${STALE_NODE}")"

agentd_smoke_stop
AGENTD_AUTH_TOKEN="${DAEMON_TOKEN}" \
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_consensus_runtime_smoke_stale_restart" \
  --db-path "${TEST_DB}" \
  --state-dir "${TEST_STATE}" \
  --tools none
DAEMON_URL="http://${HOST}:${PORT_DAEMON}"
agentd_smoke_wait_health "${DAEMON_URL}" "${DAEMON_TOKEN}"

STALE_STATUS_JSON="$(curl_json GET "/api/v1/edge/node/consensus_runtime?node_id=${STALE_NODE}")"

DRIFT_ROTATE_V1_JSON="$(curl_json POST "/api/v1/edge/consensus/membership/rotate" "$(cat <<JSON
{"cluster_id":"${DRIFT_CLUSTER}","mode":"replace","membership_epoch":21,"member_node_ids":["${DRIFT_MEMBER_A}","${DRIFT_MEMBER_B}","${DRIFT_MEMBER_C}"],"campaign_delay_ms":75,"campaign_retry_ms":400,"campaign_retry_max_ms":900,"campaign_retry_backoff_factor":2,"leader_heartbeat_ms":240,"leader_lease_ms":1200,"lease_expiry_recampaign_delay_ms":410}
JSON
)")"
DRIFT_START_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"start","node_id":"${DRIFT_NODE}","cluster_id":"${DRIFT_CLUSTER}","manifest_sha256":"${DRIFT_SHA}","deadline_ms":30000,"poll_interval_ms":100}
JSON
)")"
DRIFT_RUNNING_JSON="$(wait_runtime_running "${DRIFT_NODE}")"
DRIFT_ROTATE_V2_JSON="$(curl_json POST "/api/v1/edge/consensus/membership/rotate" "$(cat <<JSON
{"cluster_id":"${DRIFT_CLUSTER}","mode":"replace","membership_epoch":22,"member_node_ids":["${DRIFT_MEMBER_A}","${DRIFT_MEMBER_B}","${DRIFT_MEMBER_D}"],"campaign_delay_ms":125,"campaign_retry_ms":650,"campaign_retry_max_ms":1600,"campaign_retry_backoff_factor":3,"leader_heartbeat_ms":360,"leader_lease_ms":1800,"lease_expiry_recampaign_delay_ms":920}
JSON
)")"
DRIFT_STATUS_JSON="$(curl_json GET "/api/v1/edge/node/consensus_runtime?node_id=${DRIFT_NODE}")"
DRIFT_CONFLICT_RAW="$(curl_json_status POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"start","node_id":"${DRIFT_NODE}","cluster_id":"${DRIFT_CLUSTER}","manifest_sha256":"${DRIFT_SHA}","deadline_ms":30000,"poll_interval_ms":100}
JSON
)")"
DRIFT_CONFLICT_STATUS="$(printf '%s\n' "${DRIFT_CONFLICT_RAW}" | sed -n '1p')"
DRIFT_CONFLICT_JSON="$(printf '%s\n' "${DRIFT_CONFLICT_RAW}" | sed '1d')"
DRIFT_STOP_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"stop","node_id":"${DRIFT_NODE}"}
JSON
)")"
DRIFT_RESTART_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"start","node_id":"${DRIFT_NODE}","cluster_id":"${DRIFT_CLUSTER}","manifest_sha256":"${DRIFT_SHA}","deadline_ms":30000,"poll_interval_ms":100}
JSON
)")"
DRIFT_RESTART_RUNNING_JSON="$(wait_runtime_running "${DRIFT_NODE}")"
DRIFT_RESTART_STOP_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"stop","node_id":"${DRIFT_NODE}"}
JSON
)")"

TRUST_CFG_V1_JSON="$(curl_json POST "/api/v1/config/update" "$(cat <<JSON
{"edge_auth_trust_roots_epoch":5,"edge_auth_revocations_epoch":6,"edge_auth_cert_roots_epoch":7}
JSON
)")"
TRUST_START_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"start","node_id":"${TRUST_NODE}","cluster_id":"${TRUST_CLUSTER}","manifest_sha256":"${TRUST_SHA}","deadline_ms":30000,"poll_interval_ms":100}
JSON
)")"
TRUST_RUNNING_JSON="$(wait_runtime_running "${TRUST_NODE}")"
TRUST_CFG_V2_JSON="$(curl_json POST "/api/v1/config/update" "$(cat <<JSON
{"edge_auth_trust_roots_epoch":8,"edge_auth_revocations_epoch":9,"edge_auth_cert_roots_epoch":10}
JSON
)")"
TRUST_STATUS_JSON="$(curl_json GET "/api/v1/edge/node/consensus_runtime?node_id=${TRUST_NODE}")"
TRUST_CONFLICT_RAW="$(curl_json_status POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"start","node_id":"${TRUST_NODE}","cluster_id":"${TRUST_CLUSTER}","manifest_sha256":"${TRUST_SHA}","deadline_ms":30000,"poll_interval_ms":100}
JSON
)")"
TRUST_CONFLICT_STATUS="$(printf '%s\n' "${TRUST_CONFLICT_RAW}" | sed -n '1p')"
TRUST_CONFLICT_JSON="$(printf '%s\n' "${TRUST_CONFLICT_RAW}" | sed '1d')"
TRUST_STOP_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"stop","node_id":"${TRUST_NODE}"}
JSON
)")"
TRUST_RESTART_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"start","node_id":"${TRUST_NODE}","cluster_id":"${TRUST_CLUSTER}","manifest_sha256":"${TRUST_SHA}","deadline_ms":30000,"poll_interval_ms":100}
JSON
)")"
TRUST_RESTART_RUNNING_JSON="$(wait_runtime_running "${TRUST_NODE}")"
TRUST_RESTART_STOP_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"stop","node_id":"${TRUST_NODE}"}
JSON
)")"

CONFIG_SET_JSON="$(curl_json POST "/api/v1/config/update" "$(cat <<JSON
{"edge_consensus":{"node_tool_path":"${NODE_TOOL_BIN}","default_runtime_kind":"external"}}
JSON
)")"
CONFIG_GET_JSON="$(curl_json GET "/api/v1/config")"

agentd_smoke_stop
AGENTD_AUTH_TOKEN="${DAEMON_TOKEN}" \
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_consensus_runtime_smoke_restart" \
  --db-path "${TEST_DB}" \
  --state-dir "${TEST_STATE}" \
  --tools none
DAEMON_URL="http://${HOST}:${PORT_DAEMON}"
agentd_smoke_wait_health "${DAEMON_URL}" "${DAEMON_TOKEN}"

CONFIG_RESTART_JSON="$(curl_json GET "/api/v1/config")"

EXT_NODE="node_runtime_cons_ext"
EXT_PEER="node_runtime_cons_ext_peer"
EXT_CLUSTER="lab-consensus-managed-ext"
EXT_SHA="sha256:6666666666666666666666666666666666666666666666666666666666666666"
FALSE_BIN="$(python3 - <<'PY'
import shutil
print(shutil.which("false") or "")
PY
)"
if [[ -z "${FALSE_BIN}" ]]; then
  echo "missing false helper binary for fail-fast startup proof" >&2
  exit 1
fi

START_EXT_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"start","node_id":"${EXT_NODE}","cluster_id":"${EXT_CLUSTER}","manifest_sha256":"${EXT_SHA}","member_node_ids":["${EXT_NODE}","${EXT_PEER}"],"cluster_size":2,"deadline_ms":30000,"poll_interval_ms":100}
JSON
)")"
RUNNING_EXT_JSON="$(wait_runtime_running "${EXT_NODE}")"

agentd_smoke_stop
AGENTD_AUTH_TOKEN="${DAEMON_TOKEN}" \
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_consensus_runtime_smoke_external_recover" \
  --db-path "${TEST_DB}" \
  --state-dir "${TEST_STATE}" \
  --tools none
DAEMON_URL="http://${HOST}:${PORT_DAEMON}"
agentd_smoke_wait_health "${DAEMON_URL}" "${DAEMON_TOKEN}"

EXT_RESTART_STATUS_JSON="$(wait_runtime_running_persisted "${EXT_NODE}")"
python3 - <<PY
import sqlite3
conn = sqlite3.connect(r'''${TEST_DB}''')
try:
    conn.execute("DELETE FROM edge_nodes WHERE node_id = ?", (r'''${EXT_NODE}''',))
    conn.commit()
finally:
    conn.close()
PY
EXT_RESTART_NODE_JSON="$(curl_json GET "/api/v1/edge/node?node_id=${EXT_NODE}")"
EXT_RESTART_NODES_JSON="$(curl_json GET "/api/v1/edge/nodes?limit=200")"
START_EXT_AGAIN_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"start","node_id":"${EXT_NODE}","cluster_id":"${EXT_CLUSTER}","manifest_sha256":"${EXT_SHA}","member_node_ids":["${EXT_NODE}","${EXT_PEER}"],"cluster_size":2,"deadline_ms":30000,"poll_interval_ms":100}
JSON
)")"
CONFLICT_START_RAW="$(curl_json_status POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"start","node_id":"${EXT_NODE}","cluster_id":"${EXT_CLUSTER}","manifest_sha256":"${EXT_SHA}","member_node_ids":["${EXT_NODE}","${EXT_PEER}"],"cluster_size":2,"leader_lease_ms":7777,"deadline_ms":30000,"poll_interval_ms":100}
JSON
)")"
CONFLICT_START_STATUS="$(printf '%s\n' "${CONFLICT_START_RAW}" | sed -n '1p')"
CONFLICT_START_JSON="$(printf '%s\n' "${CONFLICT_START_RAW}" | sed '1d')"
STOP_EXT_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"stop","node_id":"${EXT_NODE}"}
JSON
)")"
STOP_EXT_STATUS_JSON="$(wait_runtime_stopped_with_signal "${EXT_NODE}" 15)"

FAILFAST_CFG_JSON="$(curl_json POST "/api/v1/config/update" "$(cat <<JSON
{"edge_consensus":{"node_tool_path":"${FALSE_BIN}"}}
JSON
)")"
FAILFAST_CONFIG_JSON="$(curl_json GET "/api/v1/config")"
FAILFAST_START_RAW="$(curl_json_status POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"start","node_id":"${EXT_NODE}","cluster_id":"${EXT_CLUSTER}","manifest_sha256":"${EXT_SHA}","deadline_ms":30000,"poll_interval_ms":100}
JSON
)")"
FAILFAST_START_STATUS="$(printf '%s\n' "${FAILFAST_START_RAW}" | sed -n '1p')"
FAILFAST_START_JSON="$(printf '%s\n' "${FAILFAST_START_RAW}" | sed '1d')"
FAILFAST_STATUS_JSON="$(curl_json GET "/api/v1/edge/node/consensus_runtime?node_id=${EXT_NODE}")"

CLEAR_CFG_JSON="$(curl_json POST "/api/v1/config/update" "$(cat <<JSON
{"edge_consensus":{"node_tool_path":null}}
JSON
)")"
CONFIG_CLEARED_JSON="$(curl_json GET "/api/v1/config")"
FAIL_START_RAW="$(curl_json_status POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"start","node_id":"${EXT_NODE}","cluster_id":"${EXT_CLUSTER}","manifest_sha256":"${EXT_SHA}","deadline_ms":30000,"poll_interval_ms":100}
JSON
)")"
FAIL_START_STATUS="$(printf '%s\n' "${FAIL_START_RAW}" | sed -n '1p')"
FAIL_START_JSON="$(printf '%s\n' "${FAIL_START_RAW}" | sed '1d')"
FAIL_START_STATUS_JSON="$(curl_json GET "/api/v1/edge/node/consensus_runtime?node_id=${EXT_NODE}")"

python3 - <<PY
import json
import sqlite3

db_path = r'''${TEST_DB}'''
conn = sqlite3.connect(db_path)
try:
    row = conn.execute("SELECT value FROM meta WHERE key = 'daemon.runtime_config_json'").fetchone()
    if row is None:
        raise SystemExit("missing daemon.runtime_config_json before edge consensus corruption test")
    data = json.loads(row[0])
    edge = data.setdefault("edge_consensus", {})
    edge["default_runtime_kind"] = "not-a-real-runtime-kind"
    conn.execute("UPDATE meta SET value = ? WHERE key = 'daemon.runtime_config_json'", (json.dumps(data),))
    conn.execute("UPDATE meta SET value = ? WHERE key = ?", ('{"broken"', "edge.consensus_runtime.${STOP_NODE}"))
    conn.commit()
finally:
    conn.close()
PY

agentd_smoke_stop
AGENTD_AUTH_TOKEN="${DAEMON_TOKEN}" \
agentd_smoke_start "${AGENTD_BIN}" "${HOST}" "${PORT_DAEMON}" "agentd_edge_consensus_runtime_smoke_self_heal" \
  --db-path "${TEST_DB}" \
  --state-dir "${TEST_STATE}" \
  --tools none
DAEMON_URL="http://${HOST}:${PORT_DAEMON}"
agentd_smoke_wait_health "${DAEMON_URL}" "${DAEMON_TOKEN}"

CONFIG_SELF_HEAL_JSON="$(curl_json GET "/api/v1/config")"
STOP_CORRUPT_STATUS_JSON="$(curl_json GET "/api/v1/edge/node/consensus_runtime?node_id=${STOP_NODE}")"
FALLBACK_NODE="node_runtime_cons_fallback"
FALLBACK_CLUSTER="lab-consensus-managed-fallback"
FALLBACK_SHA="sha256:7777777777777777777777777777777777777777777777777777777777777777"
FALLBACK_START_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"start","node_id":"${FALLBACK_NODE}","cluster_id":"${FALLBACK_CLUSTER}","manifest_sha256":"${FALLBACK_SHA}","deadline_ms":30000,"poll_interval_ms":100}
JSON
)")"
FALLBACK_RUNNING_JSON="$(wait_runtime_running "${FALLBACK_NODE}")"
FALLBACK_STOP_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"stop","node_id":"${FALLBACK_NODE}"}
JSON
)")"

python3 - <<PY
import json, sys
import sqlite3

node_tool = "${NODE_TOOL_BIN}"
false_bin = "${FALSE_BIN}"
db_path = r'''${TEST_DB}'''

stop_restart_status = json.loads(r'''${STOP_RESTART_STATUS_JSON}''')
stop_restart_node = json.loads(r'''${STOP_RESTART_NODE_JSON}''')
stop_restart_nodes = json.loads(r'''${STOP_RESTART_NODES_JSON}''')
start_stale = json.loads(r'''${START_STALE_JSON}''')
running_stale = json.loads(r'''${RUNNING_STALE_JSON}''')
stale_status = json.loads(r'''${STALE_STATUS_JSON}''')
drift_rotate_v1 = json.loads(r'''${DRIFT_ROTATE_V1_JSON}''')
drift_start = json.loads(r'''${DRIFT_START_JSON}''')
drift_running = json.loads(r'''${DRIFT_RUNNING_JSON}''')
drift_rotate_v2 = json.loads(r'''${DRIFT_ROTATE_V2_JSON}''')
drift_status = json.loads(r'''${DRIFT_STATUS_JSON}''')
drift_conflict = json.loads(r'''${DRIFT_CONFLICT_JSON}''')
drift_stop = json.loads(r'''${DRIFT_STOP_JSON}''')
drift_restart = json.loads(r'''${DRIFT_RESTART_JSON}''')
drift_restart_running = json.loads(r'''${DRIFT_RESTART_RUNNING_JSON}''')
drift_restart_stop = json.loads(r'''${DRIFT_RESTART_STOP_JSON}''')
trust_cfg_v1 = json.loads(r'''${TRUST_CFG_V1_JSON}''')
trust_start = json.loads(r'''${TRUST_START_JSON}''')
trust_running = json.loads(r'''${TRUST_RUNNING_JSON}''')
trust_cfg_v2 = json.loads(r'''${TRUST_CFG_V2_JSON}''')
trust_status = json.loads(r'''${TRUST_STATUS_JSON}''')
trust_conflict = json.loads(r'''${TRUST_CONFLICT_JSON}''')
trust_stop = json.loads(r'''${TRUST_STOP_JSON}''')
trust_restart = json.loads(r'''${TRUST_RESTART_JSON}''')
trust_restart_running = json.loads(r'''${TRUST_RESTART_RUNNING_JSON}''')
trust_restart_stop = json.loads(r'''${TRUST_RESTART_STOP_JSON}''')
config_set = json.loads(r'''${CONFIG_SET_JSON}''')
config_get = json.loads(r'''${CONFIG_GET_JSON}''')
config_restart = json.loads(r'''${CONFIG_RESTART_JSON}''')
start_ext = json.loads(r'''${START_EXT_JSON}''')
running_ext = json.loads(r'''${RUNNING_EXT_JSON}''')
ext_restart_status = json.loads(r'''${EXT_RESTART_STATUS_JSON}''')
ext_restart_node = json.loads(r'''${EXT_RESTART_NODE_JSON}''')
ext_restart_nodes = json.loads(r'''${EXT_RESTART_NODES_JSON}''')
start_ext_again = json.loads(r'''${START_EXT_AGAIN_JSON}''')
conflict_start = json.loads(r'''${CONFLICT_START_JSON}''')
stop_ext = json.loads(r'''${STOP_EXT_JSON}''')
stop_ext_status = json.loads(r'''${STOP_EXT_STATUS_JSON}''')
failfast_cfg = json.loads(r'''${FAILFAST_CFG_JSON}''')
failfast_config = json.loads(r'''${FAILFAST_CONFIG_JSON}''')
failfast_start = json.loads(r'''${FAILFAST_START_JSON}''')
failfast_status = json.loads(r'''${FAILFAST_STATUS_JSON}''')
clear_cfg = json.loads(r'''${CLEAR_CFG_JSON}''')
config_cleared = json.loads(r'''${CONFIG_CLEARED_JSON}''')
fail_start = json.loads(r'''${FAIL_START_JSON}''')
fail_start_status = json.loads(r'''${FAIL_START_STATUS_JSON}''')
config_self_heal = json.loads(r'''${CONFIG_SELF_HEAL_JSON}''')
stop_corrupt_status = json.loads(r'''${STOP_CORRUPT_STATUS_JSON}''')
fallback_start = json.loads(r'''${FALLBACK_START_JSON}''')
fallback_running = json.loads(r'''${FALLBACK_RUNNING_JSON}''')
fallback_stop = json.loads(r'''${FALLBACK_STOP_JSON}''')

for label, obj in (("config_set", config_set), ("config_get", config_get), ("config_restart", config_restart), ("failfast_cfg", failfast_cfg), ("failfast_config", failfast_config), ("clear_cfg", clear_cfg), ("config_cleared", config_cleared), ("config_self_heal", config_self_heal)):
  if not obj.get("ok"):
    print(label, obj, file=sys.stderr)
    raise SystemExit(1)

def find_nodes(obj, node_id):
  matches = []
  for row in obj.get("nodes") or []:
    if row.get("node_id") == node_id:
      matches.append(row)
  return matches

stop_restart_rt = stop_restart_status.get("runtime") or {}
if stop_restart_rt.get("status_source") != "persisted" or stop_restart_rt.get("runtime_kind") != "builtin":
  print("stop restart status did not recover persisted runtime snapshot", stop_restart_status, file=sys.stderr)
  raise SystemExit(1)
if stop_restart_rt.get("running"):
  print("stop restart status unexpectedly recovered a running runtime", stop_restart_status, file=sys.stderr)
  raise SystemExit(1)
stop_restart_node_rt = ((stop_restart_node.get("node") or {}).get("consensus_runtime") or {})
if stop_restart_node.get("ok") is not True:
  print("runtime-backed edge/node lookup failed for persisted builtin runtime", stop_restart_node, file=sys.stderr)
  raise SystemExit(1)
if (stop_restart_node.get("node") or {}).get("last_hello_utc_ms") != 0 or (stop_restart_node.get("node") or {}).get("last_heartbeat_utc_ms") != 0:
  print("runtime-backed persisted builtin node lookup should synthesize zero timestamps", stop_restart_node, file=sys.stderr)
  raise SystemExit(1)
if (stop_restart_node.get("node") or {}).get("has_manifest") is not False:
  print("runtime-backed persisted builtin node lookup should report has_manifest=false", stop_restart_node, file=sys.stderr)
  raise SystemExit(1)
if stop_restart_node_rt.get("status_source") != "persisted" or stop_restart_node_rt.get("runtime_kind") != "builtin" or stop_restart_node_rt.get("running"):
  print("runtime-backed edge/node lookup lost persisted builtin runtime snapshot", stop_restart_node, file=sys.stderr)
  raise SystemExit(1)
stop_restart_rows = find_nodes(stop_restart_nodes, "${STOP_NODE}")
if stop_restart_nodes.get("ok") is not True or len(stop_restart_rows) != 1:
  print("runtime-backed edge/nodes lookup failed for persisted builtin runtime", stop_restart_nodes, file=sys.stderr)
  raise SystemExit(1)
stop_restart_list_rt = (stop_restart_rows[0].get("consensus_runtime") or {})
if stop_restart_rows[0].get("last_hello_utc_ms") != 0 or stop_restart_rows[0].get("last_heartbeat_utc_ms") != 0:
  print("runtime-backed persisted builtin nodes listing should synthesize zero timestamps", stop_restart_nodes, file=sys.stderr)
  raise SystemExit(1)
if stop_restart_list_rt.get("status_source") != "persisted" or stop_restart_list_rt.get("runtime_kind") != "builtin" or stop_restart_list_rt.get("running"):
  print("runtime-backed edge/nodes lookup lost persisted builtin runtime snapshot", stop_restart_nodes, file=sys.stderr)
  raise SystemExit(1)
if start_stale.get("startup_confirmed") is not True or (start_stale.get("runtime") or {}).get("runtime_kind") != "builtin":
  print("stale runtime start wrong", start_stale, file=sys.stderr)
  raise SystemExit(1)
if not (running_stale.get("runtime") or {}).get("running"):
  print("stale runtime never entered running state", running_stale, file=sys.stderr)
  raise SystemExit(1)
cleanup_stale = stale_status.get("cleanup_on_stale_record") or {}
if stale_status.get("runtime") is not None:
  print("stale runtime restart unexpectedly left runtime snapshot behind", stale_status, file=sys.stderr)
  raise SystemExit(1)
if cleanup_stale.get("persisted_record_cleared") is not True:
  print("stale runtime restart did not clear stale persisted record", stale_status, file=sys.stderr)
  raise SystemExit(1)

for label, obj in (("drift_rotate_v1", drift_rotate_v1), ("drift_rotate_v2", drift_rotate_v2), ("drift_start", drift_start), ("drift_stop", drift_stop), ("drift_restart", drift_restart), ("drift_restart_stop", drift_restart_stop)):
  if not obj.get("ok"):
    print(label, obj, file=sys.stderr)
    raise SystemExit(1)
if drift_start.get("startup_confirmed") is not True or drift_restart.get("startup_confirmed") is not True:
  print("drift start/restart missing startup confirmation", drift_start, drift_restart, file=sys.stderr)
  raise SystemExit(1)
if not (drift_running.get("runtime") or {}).get("running"):
  print("drift runtime never entered running state", drift_running, file=sys.stderr)
  raise SystemExit(1)
drift_live = ((drift_running.get("runtime") or {}).get("live_status") or {})
if (drift_live.get("self") or {}).get("node_id") != "${DRIFT_NODE}":
  print("drift runtime missing live_status self node", drift_running, file=sys.stderr)
  raise SystemExit(1)
if sorted(drift_live.get("member_node_ids") or []) != sorted(["${DRIFT_MEMBER_A}", "${DRIFT_MEMBER_B}", "${DRIFT_MEMBER_C}"]):
  print("drift runtime live_status member set mismatch", drift_running, file=sys.stderr)
  raise SystemExit(1)
if drift_live.get("lease_expiry_recampaign_delay_ms") != 410:
  print("drift runtime live_status missing rotated v1 recampaign policy", drift_running, file=sys.stderr)
  raise SystemExit(1)
drift_rt = drift_status.get("runtime") or {}
drift_info = drift_rt.get("cluster_policy_drift") or {}
changed = set(drift_info.get("changed_fields") or [])
if drift_rt.get("running") is not True:
  print("drift runtime unexpectedly not running", drift_status, file=sys.stderr)
  raise SystemExit(1)
if drift_info.get("cluster_id") != "${DRIFT_CLUSTER}":
  print("drift status missing cluster id", drift_status, file=sys.stderr)
  raise SystemExit(1)
if not {"membership_epoch", "member_node_ids", "campaign_delay_ms", "campaign_retry_ms", "campaign_retry_max_ms", "campaign_retry_backoff_factor", "leader_heartbeat_ms", "leader_lease_ms", "lease_expiry_recampaign_delay_ms"}.issubset(changed):
  print("drift status missing expected changed fields", drift_status, file=sys.stderr)
  raise SystemExit(1)
current_policy = drift_info.get("current_policy") or {}
if current_policy.get("membership_epoch") != 22 or current_policy.get("leader_lease_ms") != 1800:
  print("drift status missing rotated current policy", drift_status, file=sys.stderr)
  raise SystemExit(1)
if current_policy.get("lease_expiry_recampaign_delay_ms") != 920:
  print("drift status missing rotated recampaign policy", drift_status, file=sys.stderr)
  raise SystemExit(1)
if sorted(current_policy.get("member_node_ids") or []) != sorted(["${DRIFT_MEMBER_A}", "${DRIFT_MEMBER_B}", "${DRIFT_MEMBER_D}"]):
  print("drift status current policy members wrong", drift_status, file=sys.stderr)
  raise SystemExit(1)
if "${DRIFT_CONFLICT_STATUS}" != "409":
  print("drift restart conflict wrong status", "${DRIFT_CONFLICT_STATUS}", drift_conflict, file=sys.stderr)
  raise SystemExit(1)
conflict_rt = drift_conflict.get("runtime") or {}
conflict_drift = conflict_rt.get("cluster_policy_drift") or {}
if drift_conflict.get("error") != "consensus runtime already running with different config":
  print("drift restart conflict wrong error", drift_conflict, file=sys.stderr)
  raise SystemExit(1)
if sorted((conflict_drift.get("current_policy") or {}).get("member_node_ids") or []) != sorted(["${DRIFT_MEMBER_A}", "${DRIFT_MEMBER_B}", "${DRIFT_MEMBER_D}"]):
  print("drift restart conflict missing current policy", drift_conflict, file=sys.stderr)
  raise SystemExit(1)
restart_rt = drift_restart.get("runtime") or {}
restart_running_rt = drift_restart_running.get("runtime") or {}
for label, rt in (("drift_restart", restart_rt), ("drift_restart_running", restart_running_rt)):
  if rt.get("membership_epoch") != 22 or rt.get("campaign_delay_ms") != 125 or rt.get("campaign_retry_ms") != 650:
    print(label, "did not adopt rotated policy", rt, file=sys.stderr)
    raise SystemExit(1)
  if rt.get("campaign_retry_max_ms") != 1600 or rt.get("campaign_retry_backoff_factor") != 3:
    print(label, "missing rotated retry policy", rt, file=sys.stderr)
    raise SystemExit(1)
  if rt.get("leader_heartbeat_ms") != 360 or rt.get("leader_lease_ms") != 1800:
    print(label, "missing rotated leader policy", rt, file=sys.stderr)
    raise SystemExit(1)
  if rt.get("lease_expiry_recampaign_delay_ms") != 920:
    print(label, "missing rotated lease-expiry recampaign policy", rt, file=sys.stderr)
    raise SystemExit(1)
  if "cluster_policy_drift" in rt:
    print(label, "unexpected drift after restart adoption", rt, file=sys.stderr)
    raise SystemExit(1)

for label, obj in (("trust_cfg_v1", trust_cfg_v1), ("trust_cfg_v2", trust_cfg_v2), ("trust_start", trust_start), ("trust_stop", trust_stop), ("trust_restart", trust_restart), ("trust_restart_stop", trust_restart_stop)):
  if not obj.get("ok"):
    print(label, obj, file=sys.stderr)
    raise SystemExit(1)
if trust_start.get("startup_confirmed") is not True or trust_restart.get("startup_confirmed") is not True:
  print("trust start/restart missing startup confirmation", trust_start, trust_restart, file=sys.stderr)
  raise SystemExit(1)
trust_start_rt = trust_start.get("runtime") or {}
trust_running_rt = trust_running.get("runtime") or {}
for label, rt in (("trust_start", trust_start_rt), ("trust_running", trust_running_rt)):
  epochs = rt.get("trust_epochs") or {}
  if epochs.get("trust_roots_epoch") != 5 or epochs.get("revocations_epoch") != 6 or epochs.get("cert_roots_epoch") != 7:
    print(label, "missing configured trust-epoch defaults", rt, file=sys.stderr)
    raise SystemExit(1)
if not trust_running_rt.get("running"):
  print("trust runtime never entered running state", trust_running, file=sys.stderr)
  raise SystemExit(1)
trust_live = trust_running_rt.get("live_status") or {}
trust_live_epochs = ((trust_live.get("self") or {}).get("trust_epochs") or {})
if trust_live_epochs.get("trust_roots_epoch") != 5 or trust_live_epochs.get("revocations_epoch") != 6 or trust_live_epochs.get("cert_roots_epoch") != 7:
  print("trust runtime missing live_status trust epochs", trust_running, file=sys.stderr)
  raise SystemExit(1)
trust_status_rt = trust_status.get("runtime") or {}
trust_drift = trust_status_rt.get("trust_epoch_drift") or {}
if trust_status_rt.get("running") is not True:
  print("trust runtime unexpectedly not running", trust_status, file=sys.stderr)
  raise SystemExit(1)
if set(trust_drift.get("changed_fields") or []) != {"trust_roots_epoch", "revocations_epoch", "cert_roots_epoch"}:
  print("trust drift missing expected changed fields", trust_status, file=sys.stderr)
  raise SystemExit(1)
current_trust = trust_drift.get("current_trust_epochs") or {}
if current_trust.get("trust_roots_epoch") != 8 or current_trust.get("revocations_epoch") != 9 or current_trust.get("cert_roots_epoch") != 10:
  print("trust drift missing rotated current epochs", trust_status, file=sys.stderr)
  raise SystemExit(1)
if "${TRUST_CONFLICT_STATUS}" != "409":
  print("trust restart conflict wrong status", "${TRUST_CONFLICT_STATUS}", trust_conflict, file=sys.stderr)
  raise SystemExit(1)
trust_conflict_rt = trust_conflict.get("runtime") or {}
if trust_conflict.get("error") != "consensus runtime already running with different config":
  print("trust restart conflict wrong error", trust_conflict, file=sys.stderr)
  raise SystemExit(1)
if (trust_conflict_rt.get("trust_epoch_drift") or {}).get("current_trust_epochs", {}).get("trust_roots_epoch") != 8:
  print("trust restart conflict missing current trust epochs", trust_conflict, file=sys.stderr)
  raise SystemExit(1)
trust_restart_rt = trust_restart.get("runtime") or {}
trust_restart_running_rt = trust_restart_running.get("runtime") or {}
for label, rt in (("trust_restart", trust_restart_rt), ("trust_restart_running", trust_restart_running_rt)):
  epochs = rt.get("trust_epochs") or {}
  if epochs.get("trust_roots_epoch") != 8 or epochs.get("revocations_epoch") != 9 or epochs.get("cert_roots_epoch") != 10:
    print(label, "did not adopt rotated trust epochs", rt, file=sys.stderr)
    raise SystemExit(1)
  if "trust_epoch_drift" in rt:
    print(label, "unexpected trust drift after restart adoption", rt, file=sys.stderr)
    raise SystemExit(1)

for label, obj in (("config_set", config_set), ("config_get", config_get), ("config_restart", config_restart)):
  edge = obj.get("edge_consensus") or {}
  if not edge.get("node_tool_path_configured"):
    print(label, "missing node_tool_path_configured", obj, file=sys.stderr)
    raise SystemExit(1)
  if not edge.get("external_available"):
    print(label, "expected external_available", obj, file=sys.stderr)
    raise SystemExit(1)
  if edge.get("default_runtime_kind") != "external" or edge.get("default_runtime_kind_source") != "config" or not edge.get("default_runtime_kind_available"):
    print(label, "wrong default runtime metadata", obj, file=sys.stderr)
    raise SystemExit(1)

rt = start_ext.get("runtime") or {}
if not start_ext.get("ok") or rt.get("runtime_kind") != "external":
  print("external start wrong", start_ext, file=sys.stderr)
  raise SystemExit(1)
if start_ext.get("startup_confirmed") is not True:
  print("external start missing startup_confirmed=true", start_ext, file=sys.stderr)
  raise SystemExit(1)
if rt.get("tool_path") != node_tool:
  print("external start missing tool_path", start_ext, file=sys.stderr)
  raise SystemExit(1)
if not (running_ext.get("runtime") or {}).get("running"):
  print("external runtime never entered running state", running_ext, file=sys.stderr)
  raise SystemExit(1)
running_ext_rt = running_ext.get("runtime") or {}
if running_ext_rt.get("runtime_kind") != "external":
  print("running external runtime kind mismatch", running_ext, file=sys.stderr)
  raise SystemExit(1)
running_ext_live = running_ext_rt.get("live_status") or {}
if (running_ext_live.get("self") or {}).get("node_id") != "${EXT_NODE}":
  print("running external runtime missing live_status self node", running_ext, file=sys.stderr)
  raise SystemExit(1)
if sorted(running_ext_live.get("member_node_ids") or []) != sorted(["${EXT_NODE}", "${EXT_PEER}"]):
  print("running external runtime live_status member set mismatch", running_ext, file=sys.stderr)
  raise SystemExit(1)
if running_ext_live.get("cluster_size") != 2:
  print("running external runtime missing live_status cluster size", running_ext, file=sys.stderr)
  raise SystemExit(1)
if (ext_restart_status.get("runtime") or {}).get("status_source") != "persisted":
  print("external restart did not recover persisted running runtime", ext_restart_status, file=sys.stderr)
  raise SystemExit(1)
if not (ext_restart_status.get("runtime") or {}).get("running"):
  print("external restart did not preserve running runtime", ext_restart_status, file=sys.stderr)
  raise SystemExit(1)
if (ext_restart_status.get("runtime") or {}).get("runtime_kind") != "external":
  print("external restart runtime kind mismatch", ext_restart_status, file=sys.stderr)
  raise SystemExit(1)
ext_restart_node_rt = ((ext_restart_node.get("node") or {}).get("consensus_runtime") or {})
if ext_restart_node.get("ok") is not True:
  print("runtime-backed edge/node lookup failed for recovered external runtime", ext_restart_node, file=sys.stderr)
  raise SystemExit(1)
if (ext_restart_node.get("node") or {}).get("last_hello_utc_ms") != 0 or (ext_restart_node.get("node") or {}).get("last_heartbeat_utc_ms") != 0:
  print("runtime-backed recovered external node lookup should synthesize zero timestamps", ext_restart_node, file=sys.stderr)
  raise SystemExit(1)
if (ext_restart_node.get("node") or {}).get("has_manifest") is not False:
  print("runtime-backed recovered external node lookup should report has_manifest=false", ext_restart_node, file=sys.stderr)
  raise SystemExit(1)
if ext_restart_node_rt.get("status_source") != "persisted" or ext_restart_node_rt.get("runtime_kind") != "external" or not ext_restart_node_rt.get("running"):
  print("runtime-backed edge/node lookup lost recovered external runtime", ext_restart_node, file=sys.stderr)
  raise SystemExit(1)
ext_restart_rows = find_nodes(ext_restart_nodes, "${EXT_NODE}")
if ext_restart_nodes.get("ok") is not True or len(ext_restart_rows) != 1:
  print("runtime-backed edge/nodes lookup failed for recovered external runtime", ext_restart_nodes, file=sys.stderr)
  raise SystemExit(1)
ext_restart_list_rt = (ext_restart_rows[0].get("consensus_runtime") or {})
if ext_restart_rows[0].get("last_hello_utc_ms") != 0 or ext_restart_rows[0].get("last_heartbeat_utc_ms") != 0:
  print("runtime-backed recovered external nodes listing should synthesize zero timestamps", ext_restart_nodes, file=sys.stderr)
  raise SystemExit(1)
if ext_restart_list_rt.get("status_source") != "persisted" or ext_restart_list_rt.get("runtime_kind") != "external" or not ext_restart_list_rt.get("running"):
  print("runtime-backed edge/nodes lookup lost recovered external runtime", ext_restart_nodes, file=sys.stderr)
  raise SystemExit(1)
if not start_ext_again.get("ok") or start_ext_again.get("already_running") is not True:
  print("identical external re-start did not stay idempotent", start_ext_again, file=sys.stderr)
  raise SystemExit(1)
if (start_ext_again.get("runtime") or {}).get("runtime_kind") != "external":
  print("identical external re-start lost runtime snapshot", start_ext_again, file=sys.stderr)
  raise SystemExit(1)
if (start_ext_again.get("runtime") or {}).get("status_source") != "persisted":
  print("identical external re-start should reuse persisted recovered runtime", start_ext_again, file=sys.stderr)
  raise SystemExit(1)
if "${CONFLICT_START_STATUS}" != "409":
  print("conflicting running start returned wrong status", "${CONFLICT_START_STATUS}", conflict_start, file=sys.stderr)
  raise SystemExit(1)
if conflict_start.get("error") != "consensus runtime already running with different config":
  print("conflicting running start wrong error", conflict_start, file=sys.stderr)
  raise SystemExit(1)
if (conflict_start.get("runtime") or {}).get("runtime_kind") != "external":
  print("conflicting running start lost existing runtime snapshot", conflict_start, file=sys.stderr)
  raise SystemExit(1)
if (conflict_start.get("runtime") or {}).get("leader_lease_ms") == 7777:
  print("conflicting running start unexpectedly mutated running runtime", conflict_start, file=sys.stderr)
  raise SystemExit(1)
if not stop_ext.get("ok") or not stop_ext.get("stopped"):
  print("external stop wrong", stop_ext, file=sys.stderr)
  raise SystemExit(1)
stop_ext_rt = stop_ext.get("runtime") or {}
if stop_ext_rt.get("status_source") != "persisted" or stop_ext_rt.get("runtime_kind") != "external":
  print("external stop lost recovered persisted runtime snapshot", stop_ext, file=sys.stderr)
  raise SystemExit(1)
if stop_ext_rt.get("running"):
  print("external stop still reported running runtime", stop_ext, file=sys.stderr)
  raise SystemExit(1)
if stop_ext_rt.get("exit_signal") != 15:
  print("external stop missing synthesized SIGTERM result", stop_ext, file=sys.stderr)
  raise SystemExit(1)
stop_ext_status_rt = stop_ext_status.get("runtime") or {}
if (stop_ext_status.get("runtime") or {}).get("running"):
  print("external runtime still running after stop", stop_ext_status, file=sys.stderr)
  raise SystemExit(1)
if stop_ext_status_rt.get("status_source") != "persisted" or stop_ext_status_rt.get("exit_signal") != 15:
  print("external stopped runtime did not persist SIGTERM result", stop_ext_status, file=sys.stderr)
  raise SystemExit(1)

for label, obj in (("failfast_cfg", failfast_cfg), ("failfast_config", failfast_config)):
  edge = obj.get("edge_consensus") or {}
  if not edge.get("node_tool_path_configured"):
    print(label, "expected configured false-bin helper", obj, file=sys.stderr)
    raise SystemExit(1)
  if not edge.get("external_available"):
    print(label, "expected executable false-bin helper to look available", obj, file=sys.stderr)
    raise SystemExit(1)
  if edge.get("default_runtime_kind") != "external" or edge.get("default_runtime_kind_source") != "config":
    print(label, "expected default external runtime metadata", obj, file=sys.stderr)
    raise SystemExit(1)

if "${FAILFAST_START_STATUS}" != "500":
  print("failfast external start returned wrong status", "${FAILFAST_START_STATUS}", failfast_start, file=sys.stderr)
  raise SystemExit(1)
if failfast_start.get("startup_confirmed") is not False:
  print("failfast external start missing startup_confirmed=false", failfast_start, file=sys.stderr)
  raise SystemExit(1)
ff_rt = failfast_start.get("runtime") or {}
if ff_rt.get("runtime_kind") != "external":
  print("failfast external start missing runtime snapshot", failfast_start, file=sys.stderr)
  raise SystemExit(1)
if ff_rt.get("tool_path") != false_bin:
  print("failfast external start wrong tool path", failfast_start, file=sys.stderr)
  raise SystemExit(1)
if ff_rt.get("running"):
  print("failfast external start left runtime running", failfast_start, file=sys.stderr)
  raise SystemExit(1)
if failfast_status.get("runtime") is not None:
  print("failfast external start left persisted runtime behind", failfast_status, file=sys.stderr)
  raise SystemExit(1)

for label, obj in (("clear_cfg", clear_cfg), ("config_cleared", config_cleared)):
  edge = obj.get("edge_consensus") or {}
  if edge.get("node_tool_path_configured"):
    print(label, "expected cleared node_tool_path", obj, file=sys.stderr)
    raise SystemExit(1)
  if edge.get("external_available"):
    print(label, "expected external unavailable after clear", obj, file=sys.stderr)
    raise SystemExit(1)
  if edge.get("external_unavailable_reason") != "edge_consensus_node_tool_path not configured":
    print(label, "wrong unavailable reason", obj, file=sys.stderr)
    raise SystemExit(1)
  if edge.get("default_runtime_kind") != "external" or edge.get("default_runtime_kind_source") != "config":
    print(label, "expected configured default external runtime", obj, file=sys.stderr)
    raise SystemExit(1)
  if edge.get("default_runtime_kind_available") is not False:
    print(label, "expected unavailable default runtime", obj, file=sys.stderr)
    raise SystemExit(1)
  if edge.get("default_runtime_kind_unavailable_reason") != "edge_consensus_node_tool_path not configured":
    print(label, "wrong default unavailable reason", obj, file=sys.stderr)
    raise SystemExit(1)

if "${FAIL_START_STATUS}" != "500":
  print("external start after clear returned wrong status", "${FAIL_START_STATUS}", fail_start, file=sys.stderr)
  raise SystemExit(1)
if fail_start.get("error") != "edge_consensus_node_tool_path not configured":
  print("external start after clear wrong error", fail_start, file=sys.stderr)
  raise SystemExit(1)
if fail_start_status.get("runtime") is not None:
  print("external failed start left runtime behind", fail_start_status, file=sys.stderr)
  raise SystemExit(1)

edge = config_self_heal.get("edge_consensus") or {}
if edge.get("default_runtime_kind") != "builtin" or edge.get("default_runtime_kind_source") != "auto":
  print("self-heal wrong default runtime metadata", config_self_heal, file=sys.stderr)
  raise SystemExit(1)
if edge.get("default_runtime_kind_available") is not True:
  print("self-heal expected available builtin default", config_self_heal, file=sys.stderr)
  raise SystemExit(1)
if edge.get("node_tool_path_configured"):
  print("self-heal unexpectedly restored tool path", config_self_heal, file=sys.stderr)
  raise SystemExit(1)
cleanup_corrupt = stop_corrupt_status.get("cleanup_on_corrupt_record") or {}
if stop_corrupt_status.get("runtime") is not None:
  print("corrupt persisted runtime record was not cleared", stop_corrupt_status, file=sys.stderr)
  raise SystemExit(1)
if cleanup_corrupt.get("persisted_record_cleared") is not True:
  print("corrupt persisted runtime record did not self-heal", stop_corrupt_status, file=sys.stderr)
  raise SystemExit(1)

conn = sqlite3.connect(db_path)
try:
  row = conn.execute("SELECT value FROM meta WHERE key = 'daemon.runtime_config_json'").fetchone()
  if row is None:
    print("missing daemon.runtime_config_json after edge consensus self-heal", file=sys.stderr)
    raise SystemExit(1)
  data = json.loads(row[0])
  edge_cfg = data.get("edge_consensus") or {}
  if edge_cfg.get("default_runtime_kind", "__missing__") is not None:
    print("edge consensus default_runtime_kind not rewritten to null", data, file=sys.stderr)
    raise SystemExit(1)
  row = conn.execute("SELECT value FROM meta WHERE key = ?", ("edge.consensus_runtime.${STOP_NODE}",)).fetchone()
  if row is None or (row[0] or "").strip():
    print("corrupt persisted runtime key not cleared", row, file=sys.stderr)
    raise SystemExit(1)
  row = conn.execute("SELECT value FROM meta WHERE key = ?", ("edge.consensus_runtime.${STALE_NODE}",)).fetchone()
  if row is None or (row[0] or "").strip():
    print("stale persisted runtime key not cleared", row, file=sys.stderr)
    raise SystemExit(1)
finally:
  conn.close()

if (fallback_start.get("runtime") or {}).get("runtime_kind") != "builtin":
  print("fallback start did not use builtin runtime", fallback_start, file=sys.stderr)
  raise SystemExit(1)
if not (fallback_running.get("runtime") or {}).get("running"):
  print("fallback runtime never entered running state", fallback_running, file=sys.stderr)
  raise SystemExit(1)
if not fallback_stop.get("ok") or not fallback_stop.get("stopped"):
  print("fallback stop wrong", fallback_stop, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_edge_consensus_runtime_smoke OK"
