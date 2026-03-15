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

STOP_NODE="node_runtime_cons_stop"
STOP_CLUSTER="lab-consensus-managed-stop"
STOP_SHA="sha256:5555555555555555555555555555555555555555555555555555555555555555"

START_STOP_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"start","node_id":"${STOP_NODE}","cluster_id":"${STOP_CLUSTER}","manifest_sha256":"${STOP_SHA}","deadline_ms":30000,"poll_interval_ms":100}
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

start_stop = json.loads(r'''${START_STOP_JSON}''')
if not start_stop.get("ok") or not isinstance(start_stop.get("runtime"), dict):
  print("start_stop wrong", start_stop, file=sys.stderr)
  raise SystemExit(1)
start_rt = start_stop.get("runtime") or {}
if start_rt.get("runtime_kind") != "builtin":
  print("start_stop runtime kind not builtin", start_stop, file=sys.stderr)
  raise SystemExit(1)
if not start_stop.get("builtin_available") or start_stop.get("default_runtime_kind") != "builtin":
  print("start_stop missing builtin metadata", start_stop, file=sys.stderr)
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

stop_resp = json.loads(r'''${STOP_RESP_JSON}''')
if not stop_resp.get("ok") or not stop_resp.get("stopped"):
  print("stop response wrong", stop_resp, file=sys.stderr)
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
if stop_status.get("external_available"):
  print("stop status unexpectedly reported external available", stop_status, file=sys.stderr)
  raise SystemExit(1)
PY

CONFIG_SET_JSON="$(curl_json POST "/api/v1/config/update" "$(cat <<JSON
{"edge_consensus":{"node_tool_path":"${NODE_TOOL_BIN}"}}
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
EXT_CLUSTER="lab-consensus-managed-ext"
EXT_SHA="sha256:6666666666666666666666666666666666666666666666666666666666666666"

START_EXT_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"start","runtime_kind":"external","node_id":"${EXT_NODE}","cluster_id":"${EXT_CLUSTER}","manifest_sha256":"${EXT_SHA}","deadline_ms":30000,"poll_interval_ms":100}
JSON
)")"
RUNNING_EXT_JSON="$(wait_runtime_running "${EXT_NODE}")"
STOP_EXT_JSON="$(curl_json POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"stop","node_id":"${EXT_NODE}"}
JSON
)")"
STOP_EXT_STATUS_JSON="$(curl_json GET "/api/v1/edge/node/consensus_runtime?node_id=${EXT_NODE}")"

CLEAR_CFG_JSON="$(curl_json POST "/api/v1/config/update" "$(cat <<JSON
{"edge_consensus":{"node_tool_path":null}}
JSON
)")"
CONFIG_CLEARED_JSON="$(curl_json GET "/api/v1/config")"
FAIL_START_RAW="$(curl_json_status POST "/api/v1/edge/node/consensus_runtime" "$(cat <<JSON
{"action":"start","runtime_kind":"external","node_id":"${EXT_NODE}","cluster_id":"${EXT_CLUSTER}","manifest_sha256":"${EXT_SHA}","deadline_ms":30000,"poll_interval_ms":100}
JSON
)")"
FAIL_START_STATUS="$(printf '%s\n' "${FAIL_START_RAW}" | sed -n '1p')"
FAIL_START_JSON="$(printf '%s\n' "${FAIL_START_RAW}" | sed '1d')"
FAIL_START_STATUS_JSON="$(curl_json GET "/api/v1/edge/node/consensus_runtime?node_id=${EXT_NODE}")"

python3 - <<PY
import json, sys

node_tool = "${NODE_TOOL_BIN}"

config_set = json.loads(r'''${CONFIG_SET_JSON}''')
config_get = json.loads(r'''${CONFIG_GET_JSON}''')
config_restart = json.loads(r'''${CONFIG_RESTART_JSON}''')
start_ext = json.loads(r'''${START_EXT_JSON}''')
running_ext = json.loads(r'''${RUNNING_EXT_JSON}''')
stop_ext = json.loads(r'''${STOP_EXT_JSON}''')
stop_ext_status = json.loads(r'''${STOP_EXT_STATUS_JSON}''')
clear_cfg = json.loads(r'''${CLEAR_CFG_JSON}''')
config_cleared = json.loads(r'''${CONFIG_CLEARED_JSON}''')
fail_start = json.loads(r'''${FAIL_START_JSON}''')
fail_start_status = json.loads(r'''${FAIL_START_STATUS_JSON}''')

for label, obj in (("config_set", config_set), ("config_get", config_get), ("config_restart", config_restart), ("clear_cfg", clear_cfg), ("config_cleared", config_cleared)):
  if not obj.get("ok"):
    print(label, obj, file=sys.stderr)
    raise SystemExit(1)

for label, obj in (("config_set", config_set), ("config_get", config_get), ("config_restart", config_restart)):
  edge = obj.get("edge_consensus") or {}
  if not edge.get("node_tool_path_configured"):
    print(label, "missing node_tool_path_configured", obj, file=sys.stderr)
    raise SystemExit(1)
  if not edge.get("external_available"):
    print(label, "expected external_available", obj, file=sys.stderr)
    raise SystemExit(1)
  if edge.get("default_runtime_kind") != "builtin" or not edge.get("default_runtime_kind_available"):
    print(label, "wrong default runtime metadata", obj, file=sys.stderr)
    raise SystemExit(1)

rt = start_ext.get("runtime") or {}
if not start_ext.get("ok") or rt.get("runtime_kind") != "external":
  print("external start wrong", start_ext, file=sys.stderr)
  raise SystemExit(1)
if rt.get("tool_path") != node_tool:
  print("external start missing tool_path", start_ext, file=sys.stderr)
  raise SystemExit(1)
if not (running_ext.get("runtime") or {}).get("running"):
  print("external runtime never entered running state", running_ext, file=sys.stderr)
  raise SystemExit(1)
if (running_ext.get("runtime") or {}).get("runtime_kind") != "external":
  print("running external runtime kind mismatch", running_ext, file=sys.stderr)
  raise SystemExit(1)
if not stop_ext.get("ok") or not stop_ext.get("stopped"):
  print("external stop wrong", stop_ext, file=sys.stderr)
  raise SystemExit(1)
if (stop_ext_status.get("runtime") or {}).get("running"):
  print("external runtime still running after stop", stop_ext_status, file=sys.stderr)
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

if "${FAIL_START_STATUS}" != "500":
  print("external start after clear returned wrong status", "${FAIL_START_STATUS}", fail_start, file=sys.stderr)
  raise SystemExit(1)
if fail_start.get("error") != "edge_consensus_node_tool_path not configured":
  print("external start after clear wrong error", fail_start, file=sys.stderr)
  raise SystemExit(1)
if fail_start_status.get("runtime") is not None:
  print("external failed start left runtime behind", fail_start_status, file=sys.stderr)
  raise SystemExit(1)
PY

echo "agentd_edge_consensus_runtime_smoke OK"
