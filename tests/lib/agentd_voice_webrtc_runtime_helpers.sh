#!/usr/bin/env bash

# Shared runtime-state helpers for WebRTC smoke tests.

wait_voice_peer_ready() {
  local session_id="$1"
  local expect_running="${2:-1}"
  local out_var="${3:-}"
  local expected_runtime_kind="${4:-bundled}"
  local expected_default_runtime_kind="${5:-bundled}"
  local expected_default_runtime_kind_source="${6:-env}"
  local expected_external_available="${7:-0}"
  local status_body=""
  for _ in $(seq 1 120); do
    status_body="$(voice_peer_status "${session_id}")"
    if python3 - <<PY
import json, sys
obj = json.loads(r'''${status_body}''')
peer = obj.get("peer")
expect_running = ${expect_running}
expected_runtime_kind = r'''${expected_runtime_kind}'''
expected_default_runtime_kind = r'''${expected_default_runtime_kind}'''
expected_default_runtime_kind_source = r'''${expected_default_runtime_kind_source}'''
expected_external_available = bool(${expected_external_available})
if obj.get("builtin_available") is not False or obj.get("bundled_available") is not True:
  raise SystemExit(1)
if obj.get("external_available") is not expected_external_available:
  raise SystemExit(1)
if obj.get("default_runtime_kind") != expected_default_runtime_kind:
  raise SystemExit(1)
if obj.get("default_runtime_kind_source") != expected_default_runtime_kind_source:
  raise SystemExit(1)
if obj.get("default_runtime_kind_available") is not True:
  raise SystemExit(1)
if obj.get("broker_url_default_configured") is not True or obj.get("broker_token_default_configured") is not True:
  raise SystemExit(1)
if not isinstance(peer, dict):
  raise SystemExit(1)
if peer.get("runtime_kind") != expected_runtime_kind:
  raise SystemExit(1)
running = bool(peer.get("running"))
ready = bool(peer.get("ready"))
if expect_running:
  raise SystemExit(0 if running and ready else 1)
raise SystemExit(0 if not running else 1)
PY
    then
      if [[ -n "${out_var}" ]]; then
        printf -v "${out_var}" '%s' "${status_body}"
      fi
      return 0
    fi
    sleep 0.1
  done
  echo "voice peer status did not reach expected state: ${status_body}" >&2
  return 1
}

wait_broker_session_deleted() {
  local session_id="$1"
  local code=""
  for _ in $(seq 1 100); do
    code="$(broker_session_status_code "${session_id}")"
    if [[ "${code}" == "404" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected deleted audio session after teardown, got status ${code}" >&2
  return 1
}

inject_stale_persisted_voice_runtime() {
  local session_id="$1"
  local runtime_kind="${2:-bundled}"
  python3 "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_record.py" write-stale \
    --db-path "${SESSION_DB_PATH}" \
    --state-dir "${STATE_DIR}" \
    --session-id "${session_id}" \
    --runtime-kind "${runtime_kind}" \
    --broker-url "${VOICE_BROKER_URL}" \
    --peer-tool "${PEER_TOOL}"
}

run_receiver_peer() {
  local session_id="$1"
  ROOT_PATH="${ROOT}" node "${SCRIPT_DIR}/lib/agentd_voice_webrtc_peer_receiver.js" \
    "http://127.0.0.1:${BROKER_PORT}" \
    "${session_id}" \
    "audio-webui-token" \
    "webui_playwright_peer" >>"${LOG_FILE}" 2>&1
}
