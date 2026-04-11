#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/lib/agentd_smoke_lib.sh
source "${SCRIPT_DIR}/lib/agentd_smoke_lib.sh"
# shellcheck source=tests/lib/agentd_voice_webrtc_daemon_client.sh
source "${SCRIPT_DIR}/lib/agentd_voice_webrtc_daemon_client.sh"
# shellcheck source=tests/lib/agentd_voice_webrtc_daemon_lifecycle.sh
source "${SCRIPT_DIR}/lib/agentd_voice_webrtc_daemon_lifecycle.sh"
# shellcheck source=tests/lib/agentd_voice_webrtc_broker_client.sh
source "${SCRIPT_DIR}/lib/agentd_voice_webrtc_broker_client.sh"
# shellcheck source=tests/lib/agentd_voice_webrtc_broker_fixture.sh
source "${SCRIPT_DIR}/lib/agentd_voice_webrtc_broker_fixture.sh"
# shellcheck source=tests/lib/agentd_voice_webrtc_runtime_helpers.sh
source "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_helpers.sh"

AGENTD_BIN="${1:-}"
if [[ -z "${AGENTD_BIN}" ]]; then
  echo "usage: $0 <agentd>" >&2
  exit 2
fi

if ! command -v node >/dev/null 2>&1; then
  echo "SKIP: node not found" >&2
  exit 77
fi
if ! command -v go >/dev/null 2>&1; then
  echo "SKIP: go not found" >&2
  exit 77
fi
if ! command -v python3 >/dev/null 2>&1; then
  echo "SKIP: python3 not found" >&2
  exit 77
fi

ROOT="$(agentd_smoke_project_root)"
PEER_TOOL="${ROOT}/tools/agentd_audio_webrtc_peer.js"
if [[ ! -f "${PEER_TOOL}" ]]; then
  echo "SKIP: peer tool not found (${PEER_TOOL})" >&2
  exit 77
fi

if ! ROOT_PATH="${ROOT}" node - <<'JS' >/dev/null 2>&1
const path = require('path');
try {
  const { chromium } = require(path.resolve(process.env.ROOT_PATH, 'ui/node_modules/playwright'));
  process.exit(chromium ? 0 : 1);
} catch (_) {
  process.exit(1);
}
JS
then
  echo "SKIP: playwright chromium dependency not available" >&2
  exit 77
fi

LOG_DIR="${ROOT}/build"
mkdir -p "${LOG_DIR}"

voice_webrtc_init_broker_fixture
RUN_LOG_DIR="${LOG_DIR}/agentd_session_voice_webrtc_peer_runtime_smoke_${PORT_DAEMON}"
mkdir -p "${RUN_LOG_DIR}"
LOG_FILE="${RUN_LOG_DIR}/agentd_session_voice_webrtc_peer_runtime_smoke.log"
trap voice_webrtc_cleanup_broker_fixture EXIT
voice_webrtc_start_broker_fixture

HOST="127.0.0.1"
DAEMON_TOKEN="agentd-audio-runtime-token"

start_agentd_with_voice_defaults
DAEMON_URL="http://${HOST}:${PORT_DAEMON}"

wait_daemon_ready

config_env_defaults="$(config_get)"

voice_webrtc_assert_fields "${config_env_defaults}" "env defaults config" \
  --true daemon.audio_webrtc.broker_url_default_configured \
  --true daemon.audio_webrtc.broker_token_default_configured \
  --equals daemon.audio_webrtc.default_runtime_kind=bundled \
  --equals daemon.audio_webrtc.default_runtime_kind_source=env

restart_agentd_with_builtin_voice_default

config_env_builtin_default="$(config_get)"

voice_webrtc_assert_fields "${config_env_builtin_default}" "env builtin default config" \
  --equals daemon.audio_webrtc.default_runtime_kind=builtin \
  --equals daemon.audio_webrtc.default_runtime_kind_source=env \
  --false daemon.audio_webrtc.default_runtime_kind_available \
  --false daemon.audio_webrtc.builtin_available \
  --true daemon.audio_webrtc.bundled_available \
  --contains daemon.audio_webrtc.builtin_unavailable_reason=disabled \
  --contains daemon.audio_webrtc.default_runtime_kind_unavailable_reason=disabled

ENV_BUILTIN_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_env_builtin_$(date +%s)_$RANDOM"
create_session "${ENV_BUILTIN_SESSION_ID}"

env_builtin_start_body="${RUN_LOG_DIR}/voice_webrtc_peer_env_builtin_default_body.json"
env_builtin_start_code="$(voice_peer_request_status "${env_builtin_start_body}" "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${ENV_BUILTIN_SESSION_ID}",
  "action": "start",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-env-builtin-default",
  "sender_tag": "agentd_runtime_peer"
}))
PY
)")"
if [[ "${env_builtin_start_code}" != "501" ]]; then
  echo "expected env builtin default start to return http 501, got ${env_builtin_start_code}" >&2
  cat "${env_builtin_start_body}" >&2
  exit 1
fi

voice_webrtc_assert_body_fields "${env_builtin_start_body}" "env builtin default start response" \
  --false ok \
  --equals default_runtime_kind=builtin \
  --equals default_runtime_kind_source=env \
  --false default_runtime_kind_available \
  --contains error=disabled

delete_session_quiet "${ENV_BUILTIN_SESSION_ID}"

restart_agentd

SESSION_DB_ID="agentd_session_voice_webrtc_peer_runtime_$(date +%s)_$RANDOM"
create_session "${SESSION_DB_ID}"

start_resp="$(voice_peer_request "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${SESSION_DB_ID}",
  "action": "start",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 60000,
  "poll_interval_ms": 100,
  "tone_hz": 440
}))
PY
)")"

voice_webrtc_assert_started "${start_resp}" "initial bundled start response" \
  --runtime-kind bundled \
  --managed true \
  --broker-agent-id a-1 \
  --broker-deployment-id lab \
  --peer-running true \
  --broker-defaults true \
  --builtin-available false \
  --bundled-available true \
  --external-available false \
  --default-runtime-kind bundled \
  --default-runtime-kind-source env \
  --default-runtime-kind-available true

BROKER_SESSION_ID="$(voice_webrtc_peer_field "${start_resp}" broker_session_id "start response" --require-ok --nonempty)"

status_json=""
wait_voice_peer_ready "${SESSION_DB_ID}" 1 status_json

restart_agentd
wait_voice_peer_ready "${SESSION_DB_ID}" 1 status_json

voice_webrtc_assert_fields "${status_json}" "recovered initial status" \
  --equals peer.status_source=persisted \
  --nonempty peer.stdout_log_path \
  --true peer.managed_broker_session \
  --equals peer.broker_agent_id=a-1 \
  --equals peer.broker_deployment_id=lab \
  --equals peer.runtime_kind=bundled \
  --true peer.running \
  --true peer.ready

already_running_resp="$(voice_peer_request "{\"session_id\":\"${SESSION_DB_ID}\",\"action\":\"start\"}")"

voice_webrtc_assert_fields "${already_running_resp}" "already-running response after restart" \
  --require-ok \
  --true already_running \
  --equals peer.status_source=persisted

persisted_conflict_body="${RUN_LOG_DIR}/voice_webrtc_peer_persisted_conflict_body.json"
persisted_conflict_status="$(voice_peer_request_status "${persisted_conflict_body}" "{\"session_id\":\"${SESSION_DB_ID}\",\"action\":\"start\",\"runtime_kind\":\"builtin\"}")"
if [[ "${persisted_conflict_status}" != "409" ]]; then
  echo "expected persisted running builtin conflict to return 409, got ${persisted_conflict_status}" >&2
  cat "${persisted_conflict_body}" >&2
  exit 1
fi
voice_webrtc_assert_body_fields "${persisted_conflict_body}" "persisted running conflict response" \
  --equals error="voice peer already running with different config" \
  --equals peer.status_source=persisted \
  --equals peer.runtime_kind=bundled

run_receiver_peer "${BROKER_SESSION_ID}"

SESSION_JSON="$(broker_session_get "${BROKER_SESSION_ID}")"

voice_webrtc_assert_fields "${SESSION_JSON}" "broker session after receiver" \
  --require-ok \
  --min session.signal_count=4

broker_session_signal "${BROKER_SESSION_ID}" '{"type":"bye","payload":{"reason":"webui_done","sender_tag":"webui_playwright_peer"}}'

stopped_json=""
wait_voice_peer_ready "${SESSION_DB_ID}" 0 stopped_json

python3 - <<PY
import json, sys
obj = json.loads(r'''${stopped_json}''')
peer = obj.get("peer") or {}
last = peer.get("last_stdout") or {}
if peer.get("running"):
  print("peer still running", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("exit_code") not in (0, None):
  print("unexpected exit_code", obj, file=sys.stderr)
  raise SystemExit(1)
if not last.get("closed_by_remote"):
  print("expected closed_by_remote final state", obj, file=sys.stderr)
  raise SystemExit(1)
PY

wait_broker_session_deleted "${BROKER_SESSION_ID}"

builtin_resp_body="${RUN_LOG_DIR}/voice_webrtc_peer_builtin_body.json"
builtin_status="$(voice_peer_request_status "${builtin_resp_body}" "{\"session_id\":\"${SESSION_DB_ID}\",\"action\":\"start\",\"runtime_kind\":\"builtin\",\"broker_agent_id\":\"a-1\",\"broker_deployment_id\":\"lab-builtin-contract\"}")"
if [[ "${builtin_status}" != "501" ]]; then
  echo "expected builtin runtime request to return 501, got ${builtin_status}" >&2
  cat "${builtin_resp_body}" >&2
  exit 1
fi
python3 "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_assertions.py" assert-builtin-auto-create-contract \
  --body-path "${builtin_resp_body}" \
  --broker-agent-id "a-1" \
  --broker-deployment-id "lab-builtin-contract"

BUILTIN_BORROWED_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_builtin_borrowed_$(date +%s)_$RANDOM"
create_session "${BUILTIN_BORROWED_SESSION_ID}"

builtin_borrowed_broker_create_resp="$(broker_session_create '{"agent_id":"a-1","mode":"webrtc"}')"

BUILTIN_BORROWED_BROKER_SESSION_ID="$(voice_webrtc_json_field "${builtin_borrowed_broker_create_resp}" session_id "builtin borrowed broker create response" --require-ok --nonempty)"

builtin_borrowed_resp_body="${RUN_LOG_DIR}/voice_webrtc_peer_builtin_borrowed_body.json"
builtin_borrowed_status="$(voice_peer_request_status "${builtin_borrowed_resp_body}" "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${BUILTIN_BORROWED_SESSION_ID}",
  "action": "start",
  "runtime_kind": "builtin",
  "broker_session_id": "${BUILTIN_BORROWED_BROKER_SESSION_ID}",
  "broker_url": "${VOICE_BROKER_URL}",
  "broker_token": "${VOICE_BROKER_TOKEN}",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 551
}))
PY
)")"
if [[ "${builtin_borrowed_status}" != "501" ]]; then
  echo "expected builtin borrowed start to return 501, got ${builtin_borrowed_status}" >&2
  cat "${builtin_borrowed_resp_body}" >&2
  exit 1
fi
python3 "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_assertions.py" assert-builtin-borrowed-contract \
  --body-path "${builtin_borrowed_resp_body}" \
  --broker-session-id "${BUILTIN_BORROWED_BROKER_SESSION_ID}"

builtin_borrowed_delete_status="$(broker_session_delete_status "${BUILTIN_BORROWED_BROKER_SESSION_ID}")"
if [[ "${builtin_borrowed_delete_status}" != "200" && "${builtin_borrowed_delete_status}" != "404" ]]; then
  echo "expected builtin borrowed broker session delete to return 200 or 404, got ${builtin_borrowed_delete_status}" >&2
  exit 1
fi

BUILTIN_MISSING_BROKER_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_builtin_missing_broker_$(date +%s)_$RANDOM"
create_session "${BUILTIN_MISSING_BROKER_SESSION_ID}"

builtin_missing_broker_session_resp_body="${RUN_LOG_DIR}/voice_webrtc_peer_builtin_missing_broker_session_body.json"
builtin_missing_broker_session_status="$(voice_peer_request_status "${builtin_missing_broker_session_resp_body}" "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${BUILTIN_MISSING_BROKER_SESSION_ID}",
  "action": "start",
  "runtime_kind": "builtin",
  "broker_session_id": "missing-builtin-broker-session",
  "sender_tag": "agentd_runtime_peer"
}))
PY
)")"
if [[ "${builtin_missing_broker_session_status}" != "400" ]]; then
  echo "expected builtin missing broker_session_id start to return 400, got ${builtin_missing_broker_session_status}" >&2
  cat "${builtin_missing_broker_session_resp_body}" >&2
  exit 1
fi
python3 "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_assertions.py" assert-error-response \
  --body-path "${builtin_missing_broker_session_resp_body}" \
  --label "builtin missing broker_session_id start" \
  --error-contains "broker_session_id not found"

builtin_missing_broker_session_status_json="$(voice_peer_status "${BUILTIN_MISSING_BROKER_SESSION_ID}")"

python3 "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_assertions.py" assert-no-runtime \
  --label "builtin missing broker_session_id preflight failure" \
  --response-json "${builtin_missing_broker_session_status_json}"

BUILTIN_CONFLICT_BROKER_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_builtin_conflict_broker_$(date +%s)_$RANDOM"
create_session "${BUILTIN_CONFLICT_BROKER_SESSION_ID}"

builtin_conflict_broker_session_resp_body="${RUN_LOG_DIR}/voice_webrtc_peer_builtin_conflict_broker_session_body.json"
builtin_conflict_broker_session_status="$(voice_peer_request_status "${builtin_conflict_broker_session_resp_body}" "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${BUILTIN_CONFLICT_BROKER_SESSION_ID}",
  "action": "start",
  "runtime_kind": "builtin",
  "broker_session_id": "aud_conflict_builtin",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-builtin-conflict",
  "sender_tag": "agentd_runtime_peer"
}))
PY
)")"
if [[ "${builtin_conflict_broker_session_status}" != "400" ]]; then
  echo "expected conflicting builtin broker session start to return 400, got ${builtin_conflict_broker_session_status}" >&2
  cat "${builtin_conflict_broker_session_resp_body}" >&2
  exit 1
fi
python3 "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_assertions.py" assert-error-response \
  --body-path "${builtin_conflict_broker_session_resp_body}" \
  --label "conflicting builtin broker session start" \
  --error-contains "must be omitted"

builtin_conflict_broker_session_status_json="$(voice_peer_status "${BUILTIN_CONFLICT_BROKER_SESSION_ID}")"

python3 "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_assertions.py" assert-no-runtime \
  --label "conflicting builtin broker session validation failure" \
  --response-json "${builtin_conflict_broker_session_status_json}"

external_resp_body="${RUN_LOG_DIR}/voice_webrtc_peer_external_body.json"
external_status="$(voice_peer_request_status "${external_resp_body}" "{\"session_id\":\"${SESSION_DB_ID}\",\"action\":\"start\",\"runtime_kind\":\"external\",\"broker_session_id\":\"external-test-session\"}")"
if [[ "${external_status}" != "500" ]]; then
  echo "expected explicit external runtime request to return 500 without configured tool, got ${external_status}" >&2
  cat "${external_resp_body}" >&2
  exit 1
fi
voice_webrtc_assert_body_fields "${external_resp_body}" "external unavailable contract response" \
  --false builtin_available \
  --true bundled_available \
  --false external_available \
  --equals default_runtime_kind=bundled \
  --equals default_runtime_kind_source=env \
  --true default_runtime_kind_available \
  --true broker_url_default_configured \
  --true broker_token_default_configured \
  --contains external_unavailable_reason="not configured" \
  --contains error="not configured"

missing_broker_session_resp_body="${RUN_LOG_DIR}/voice_webrtc_peer_missing_broker_session_body.json"
MISSING_BROKER_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_missing_broker_$(date +%s)_$RANDOM"
create_session "${MISSING_BROKER_SESSION_ID}"

missing_broker_session_status="$(voice_peer_request_status "${missing_broker_session_resp_body}" "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${MISSING_BROKER_SESSION_ID}",
  "action": "start",
  "broker_session_id": "missing-broker-session",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 517
}))
PY
)")"
if [[ "${missing_broker_session_status}" != "400" ]]; then
  echo "expected missing broker_session_id start to return 400, got ${missing_broker_session_status}" >&2
  cat "${missing_broker_session_resp_body}" >&2
  exit 1
fi
python3 "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_assertions.py" assert-error-response \
  --body-path "${missing_broker_session_resp_body}" \
  --label "missing broker_session_id start" \
  --error-contains "broker_session_id not found"

missing_broker_session_status_json="$(voice_peer_status "${MISSING_BROKER_SESSION_ID}")"

python3 "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_assertions.py" assert-no-runtime \
  --label "missing broker_session_id preflight failure" \
  --response-json "${missing_broker_session_status_json}"

delete_session_quiet "${MISSING_BROKER_SESSION_ID}"

CONFLICT_BROKER_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_conflict_broker_$(date +%s)_$RANDOM"
create_session "${CONFLICT_BROKER_SESSION_ID}"

conflict_broker_session_resp_body="${RUN_LOG_DIR}/voice_webrtc_peer_conflict_broker_session_body.json"
conflict_broker_session_status="$(voice_peer_request_status "${conflict_broker_session_resp_body}" "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${CONFLICT_BROKER_SESSION_ID}",
  "action": "start",
  "broker_session_id": "aud_conflict",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-conflict",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 518
}))
PY
)")"
if [[ "${conflict_broker_session_status}" != "400" ]]; then
  echo "expected conflicting broker session start to return 400, got ${conflict_broker_session_status}" >&2
  cat "${conflict_broker_session_resp_body}" >&2
  exit 1
fi
python3 "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_assertions.py" assert-error-response \
  --body-path "${conflict_broker_session_resp_body}" \
  --label "conflicting broker session start" \
  --error-contains "must be omitted"

conflict_broker_session_status_json="$(voice_peer_status "${CONFLICT_BROKER_SESSION_ID}")"

python3 "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_assertions.py" assert-no-runtime \
  --label "conflicting broker session validation failure" \
  --response-json "${conflict_broker_session_status_json}"

delete_session_quiet "${CONFLICT_BROKER_SESSION_ID}"

start_resp2="$(voice_peer_request "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${SESSION_DB_ID}",
  "action": "start",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-stop",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 523
}))
PY
)")"

voice_webrtc_assert_started "${start_resp2}" "second start response" \
  --runtime-kind bundled \
  --managed true \
  --broker-deployment-id lab-stop

BROKER_SESSION_ID2="$(voice_webrtc_peer_field "${start_resp2}" broker_session_id "second start response" --require-ok --nonempty)"

VOICE_PEER_PID2="$(voice_webrtc_peer_field "${start_resp2}" pid "second start response" --require-ok --positive-int)"

wait_voice_peer_ready "${SESSION_DB_ID}" 1 status_json

kill -9 "${VOICE_PEER_PID2}"

session_exists_status="$(broker_session_status_code "${BROKER_SESSION_ID2}")"
if [[ "${session_exists_status}" != "200" ]]; then
  echo "expected broker audio session to still exist after peer SIGKILL, got ${session_exists_status}" >&2
  exit 1
fi

restart_agentd
wait_voice_peer_ready "${SESSION_DB_ID}" 0 stopped_json

voice_webrtc_assert_fields "${stopped_json}" "forced-exit restart status" \
  --equals peer.status_source=persisted \
  --false peer.running \
  --equals peer.exit_signal=9

stop_resp="$(voice_peer_request "{\"session_id\":\"${SESSION_DB_ID}\",\"action\":\"stop\"}")"

voice_webrtc_assert_stopped "${stop_resp}" "forced-exit stop response" \
  --runtime-kind bundled \
  --stopped false \
  --reason not_running \
  --broker-session-deleted true

wait_voice_peer_ready "${SESSION_DB_ID}" 0 stopped_json

voice_webrtc_assert_fields "${stopped_json}" "forced-exit cleanup status" \
  --false peer.running \
  --equals peer.status_source=persisted \
  --equals peer.exit_signal=9

wait_broker_session_deleted "${BROKER_SESSION_ID2}"

RECOVERED_STOP_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_recovered_stop_$(date +%s)_$RANDOM"
create_session "${RECOVERED_STOP_SESSION_ID}"

recovered_stop_start_resp="$(voice_peer_request "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${RECOVERED_STOP_SESSION_ID}",
  "action": "start",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-recovered-stop",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 587
}))
PY
)")"

voice_webrtc_assert_started "${recovered_stop_start_resp}" "recovered-stop start response" \
  --runtime-kind bundled \
  --managed true

RECOVERED_STOP_BROKER_SESSION_ID="$(voice_webrtc_peer_field "${recovered_stop_start_resp}" broker_session_id "recovered-stop start response" --require-ok --nonempty)"

wait_voice_peer_ready "${RECOVERED_STOP_SESSION_ID}" 1 status_json

restart_agentd
wait_voice_peer_ready "${RECOVERED_STOP_SESSION_ID}" 1 status_json

voice_webrtc_assert_fields "${status_json}" "recovered-stop restart status" \
  --equals peer.status_source=persisted \
  --equals peer.runtime_kind=bundled \
  --true peer.running \
  --true peer.ready

recovered_stop_resp="$(voice_peer_request "{\"session_id\":\"${RECOVERED_STOP_SESSION_ID}\",\"action\":\"stop\"}")"

voice_webrtc_assert_stopped "${recovered_stop_resp}" "recovered-stop response" \
  --runtime-kind bundled \
  --stopped true \
  --broker-session-deleted true \
  --peer-running false \
  --status-source persisted \
  --exit-signal 15

wait_voice_peer_ready "${RECOVERED_STOP_SESSION_ID}" 0 stopped_json

voice_webrtc_assert_fields "${stopped_json}" "recovered-stop final status" \
  --equals peer.status_source=persisted \
  --false peer.running \
  --equals peer.exit_signal=15

wait_broker_session_deleted "${RECOVERED_STOP_BROKER_SESSION_ID}"

RECOVERED_DELETE_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_recovered_delete_$(date +%s)_$RANDOM"
create_session "${RECOVERED_DELETE_SESSION_ID}"

recovered_delete_start_resp="$(voice_peer_request "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${RECOVERED_DELETE_SESSION_ID}",
  "action": "start",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-recovered-delete",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 611
}))
PY
)")"

voice_webrtc_assert_started "${recovered_delete_start_resp}" "recovered-delete start response" \
  --runtime-kind bundled \
  --managed true

RECOVERED_DELETE_BROKER_SESSION_ID="$(voice_webrtc_peer_field "${recovered_delete_start_resp}" broker_session_id "recovered-delete start response" --require-ok --nonempty)"

RECOVERED_DELETE_STDOUT_LOG="$(voice_webrtc_peer_field "${recovered_delete_start_resp}" stdout_log_path "recovered-delete start response" --require-ok --nonempty)"

wait_voice_peer_ready "${RECOVERED_DELETE_SESSION_ID}" 1 status_json

restart_agentd
wait_voice_peer_ready "${RECOVERED_DELETE_SESSION_ID}" 1 status_json

voice_webrtc_assert_fields "${status_json}" "recovered-delete restart status" \
  --equals peer.status_source=persisted \
  --equals peer.runtime_kind=bundled \
  --true peer.running \
  --true peer.ready

recovered_delete_resp="$(delete_session "${RECOVERED_DELETE_SESSION_ID}")"

voice_webrtc_assert_fields "${recovered_delete_resp}" "recovered-delete session delete cleanup" \
  --require-ok \
  --true deleted_from_db \
  --true voice_runtime_cleanup.runtime_present \
  --true voice_runtime_cleanup.runtime_was_running \
  --true voice_runtime_cleanup.stopped \
  --true voice_runtime_cleanup.broker_session_delete_attempted \
  --true voice_runtime_cleanup.broker_session_deleted \
  --true voice_runtime_cleanup.persisted_record_cleared \
  --true voice_runtime_cleanup.runtime_artifacts_deleted \
  --equals voice_runtime_cleanup.peer.status_source=persisted \
  --equals voice_runtime_cleanup.peer.runtime_kind=bundled \
  --false voice_runtime_cleanup.peer.running \
  --equals voice_runtime_cleanup.peer.exit_signal=15

recovered_delete_status="$(voice_peer_status "${RECOVERED_DELETE_SESSION_ID}")"

python3 - <<PY
import json, os, sys
obj = json.loads(r'''${recovered_delete_status}''')
if obj.get("session_exists") is not False or obj.get("running") is not False or obj.get("peer") is not None:
  print("expected no peer state after recovered-delete cleanup", obj, file=sys.stderr)
  raise SystemExit(1)
if os.path.exists(r'''${RECOVERED_DELETE_STDOUT_LOG}'''):
  print("expected recovered-delete stdout log to be removed", r'''${RECOVERED_DELETE_STDOUT_LOG}''', file=sys.stderr)
  raise SystemExit(1)
PY

restart_agentd

recovered_delete_status_restart="$(voice_peer_status "${RECOVERED_DELETE_SESSION_ID}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${recovered_delete_status_restart}''')
if obj.get("session_exists") is not False or obj.get("running") is not False or obj.get("peer") is not None:
  print("expected no resurrected peer state after recovered-delete restart", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("cleanup_on_missing_session") not in (None, {}):
  print("unexpected cleanup_on_missing_session after recovered-delete cleanup", obj, file=sys.stderr)
  raise SystemExit(1)
PY

wait_broker_session_deleted "${RECOVERED_DELETE_BROKER_SESSION_ID}"

start_resp3="$(voice_peer_request "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${SESSION_DB_ID}",
  "action": "start",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-delete",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 659
}))
PY
)")"

voice_webrtc_assert_started "${start_resp3}" "third start response" \
  --runtime-kind bundled \
  --managed true \
  --broker-deployment-id lab-delete

BROKER_SESSION_ID3="$(voice_webrtc_peer_field "${start_resp3}" broker_session_id "third start response" --require-ok --nonempty)"

VOICE_STDOUT_LOG3="$(voice_webrtc_peer_field "${start_resp3}" stdout_log_path "third start response" --require-ok --nonempty)"

wait_voice_peer_ready "${SESSION_DB_ID}" 1 status_json

delete_resp="$(delete_session "${SESSION_DB_ID}")"

voice_webrtc_assert_fields "${delete_resp}" "session delete runtime cleanup" \
  --require-ok \
  --true deleted_from_db \
  --true voice_runtime_cleanup.runtime_present \
  --true voice_runtime_cleanup.runtime_was_running \
  --true voice_runtime_cleanup.broker_session_delete_attempted \
  --true voice_runtime_cleanup.broker_session_deleted \
  --true voice_runtime_cleanup.persisted_record_cleared

session_after_delete_body="${RUN_LOG_DIR}/voice_runtime_session_after_delete.json"
session_after_delete_status="$(session_status_code "${SESSION_DB_ID}" "${session_after_delete_body}")"
if [[ "${session_after_delete_status}" != "404" ]]; then
  echo "expected deleted session to return 404, got ${session_after_delete_status}" >&2
  cat "${session_after_delete_body}" >&2
  exit 1
fi

status_after_delete="$(voice_peer_status "${SESSION_DB_ID}")"

python3 - <<PY
import json, os, sys
obj = json.loads(r'''${status_after_delete}''')
if obj.get("session_exists") is not False:
  print("expected session_exists=false after delete", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("running") is not False:
  print("expected running=false after delete", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("peer") is not None:
  print("expected peer=null after delete cleanup", obj, file=sys.stderr)
  raise SystemExit(1)
if os.path.exists(r'''${VOICE_STDOUT_LOG3}'''):
  print("expected stdout log to be removed after session delete", r'''${VOICE_STDOUT_LOG3}''', file=sys.stderr)
  raise SystemExit(1)
PY

restart_agentd

status_after_delete_restart="$(voice_peer_status "${SESSION_DB_ID}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${status_after_delete_restart}''')
if obj.get("session_exists") is not False or obj.get("running") is not False or obj.get("peer") is not None:
  print("expected no resurrected peer state after restart", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("cleanup_on_missing_session") not in (None, {}):
  print("unexpected cleanup_on_missing_session after already-clean delete", obj, file=sys.stderr)
  raise SystemExit(1)
PY

wait_broker_session_deleted "${BROKER_SESSION_ID3}"

STALE_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_stale_$(date +%s)_$RANDOM"
create_session "${STALE_SESSION_ID}"

start_resp4="$(voice_peer_request "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${STALE_SESSION_ID}",
  "action": "start",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-stale",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 784
}))
PY
)")"

voice_webrtc_assert_started "${start_resp4}" "fourth start response" \
  --runtime-kind bundled \
  --managed true

BROKER_SESSION_ID4="$(voice_webrtc_peer_field "${start_resp4}" broker_session_id "fourth start response" --require-ok --nonempty)"

VOICE_STDOUT_LOG4="$(voice_webrtc_peer_field "${start_resp4}" stdout_log_path "fourth start response" --require-ok --nonempty)"

wait_voice_peer_ready "${STALE_SESSION_ID}" 1 status_json

python3 "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_record.py" delete-session \
  --db-path "${SESSION_DB_PATH}" \
  --session-id "${STALE_SESSION_ID}"

status_after_stale_delete="$(voice_peer_status "${STALE_SESSION_ID}")"

python3 - <<PY
import json, os, sys
obj = json.loads(r'''${status_after_stale_delete}''')
if obj.get("session_exists") is not False:
  print("expected session_exists=false after direct DB delete", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("running") is not False:
  print("expected running=false after stale cleanup", obj, file=sys.stderr)
  raise SystemExit(1)
if obj.get("peer") is not None:
  print("expected peer=null after stale cleanup", obj, file=sys.stderr)
  raise SystemExit(1)
cleanup = obj.get("cleanup_on_missing_session") or {}
if cleanup.get("runtime_present") is not True or cleanup.get("runtime_was_running") is not True:
  print("expected cleanup_on_missing_session to report stale running runtime", obj, file=sys.stderr)
  raise SystemExit(1)
if cleanup.get("persisted_record_cleared") is not True:
  print("expected persisted runtime record cleared during stale cleanup", obj, file=sys.stderr)
  raise SystemExit(1)
if os.path.exists(r'''${VOICE_STDOUT_LOG4}'''):
  print("expected stale runtime stdout log removed", r'''${VOICE_STDOUT_LOG4}''', file=sys.stderr)
  raise SystemExit(1)
PY

wait_broker_session_deleted "${BROKER_SESSION_ID4}"

config_update_resp="$(config_update "$(python3 - <<PY
import json
print(json.dumps({
  "audio_webrtc": {
    "broker_url": "${VOICE_BROKER_URL}",
    "broker_token": "${VOICE_BROKER_TOKEN}",
    "peer_tool_path": "${PEER_TOOL}",
    "default_runtime_kind": "external",
    "node_bin": "node"
  }
}))
PY
)")"

voice_webrtc_assert_fields "${config_update_resp}" "audio WebRTC config update" \
  --require-ok \
  --true audio_webrtc.broker_url_default_configured \
  --true audio_webrtc.broker_token_default_configured \
  --true audio_webrtc.peer_tool_path_configured \
  --equals audio_webrtc.default_runtime_kind=external \
  --equals audio_webrtc.default_runtime_kind_source=config \
  --equals audio_webrtc.node_bin=node

restart_agentd_without_voice_defaults

config_persisted_defaults="$(config_get)"

voice_webrtc_assert_fields "${config_persisted_defaults}" "persisted audio WebRTC config" \
  --true daemon.audio_webrtc.broker_url_default_configured \
  --true daemon.audio_webrtc.broker_token_default_configured \
  --true daemon.audio_webrtc.peer_tool_path_configured \
  --equals daemon.audio_webrtc.default_runtime_kind=external \
  --equals daemon.audio_webrtc.default_runtime_kind_source=config \
  --equals daemon.audio_webrtc.node_bin=node

CONFIG_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_cfg_$(date +%s)_$RANDOM"
create_session "${CONFIG_SESSION_ID}"

config_start_resp="$(voice_peer_request "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${CONFIG_SESSION_ID}",
  "action": "start",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-config",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 880
}))
PY
)")"

voice_webrtc_assert_started "${config_start_resp}" "config-backed start response" \
  --runtime-kind external \
  --managed true \
  --broker-deployment-id lab-config \
  --broker-defaults true \
  --external-available true \
  --default-runtime-kind external \
  --default-runtime-kind-source config \
  --default-runtime-kind-available true

CONFIG_BROKER_SESSION_ID="$(voice_webrtc_peer_field "${config_start_resp}" broker_session_id "config-backed start response" --require-ok --nonempty)"

wait_voice_peer_ready "${CONFIG_SESSION_ID}" 1 status_json external external config 1

config_stop_resp="$(voice_peer_request "{\"session_id\":\"${CONFIG_SESSION_ID}\",\"action\":\"stop\"}")"

voice_webrtc_assert_stopped "${config_stop_resp}" "config-backed stop response" \
  --stopped true \
  --broker-session-deleted true

wait_broker_session_deleted "${CONFIG_BROKER_SESSION_ID}"

delete_session_quiet "${CONFIG_SESSION_ID}"

EXTERNAL_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_external_$(date +%s)_$RANDOM"
create_session "${EXTERNAL_SESSION_ID}"

external_config_start_resp="$(voice_peer_request "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${EXTERNAL_SESSION_ID}",
  "action": "start",
  "runtime_kind": "external",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-external-config",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 901
}))
PY
)")"

voice_webrtc_assert_started "${external_config_start_resp}" "external config-backed start response" \
  --runtime-kind external \
  --managed true \
  --broker-deployment-id lab-external-config

EXTERNAL_BROKER_SESSION_ID="$(voice_webrtc_peer_field "${external_config_start_resp}" broker_session_id "external config-backed start response" --require-ok --nonempty)"

wait_voice_peer_ready "${EXTERNAL_SESSION_ID}" 1 status_json external external config 1

external_config_start_again_resp="$(voice_peer_request "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${EXTERNAL_SESSION_ID}",
  "action": "start",
  "runtime_kind": "external",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-external-config",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 901
}))
PY
)")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${external_config_start_again_resp}''')
peer = obj.get("peer") or {}
if not obj.get("ok") or obj.get("already_running") is not True:
  print("expected idempotent already_running response for external runtime", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("runtime_kind") != "external" or peer.get("tone_hz") != 901:
  print("unexpected peer snapshot for idempotent external start", obj, file=sys.stderr)
  raise SystemExit(1)
PY

external_default_conflict_update_resp="$(config_update "$(python3 - <<PY
import json
print(json.dumps({
  "audio_webrtc": {
    "broker_url": "${VOICE_BROKER_URL}",
    "broker_token": "${VOICE_BROKER_TOKEN}",
    "peer_tool_path": "${PEER_TOOL}",
    "default_runtime_kind": "bundled",
    "node_bin": "node"
  }
}))
PY
)")"

voice_webrtc_assert_fields "${external_default_conflict_update_resp}" "external default conflict config" \
  --require-ok \
  --equals audio_webrtc.default_runtime_kind=bundled \
  --equals audio_webrtc.default_runtime_kind_source=config \
  --true audio_webrtc.bundled_available \
  --true audio_webrtc.external_available

external_default_conflict_status_resp="$(voice_peer_status "${EXTERNAL_SESSION_ID}")"

voice_webrtc_assert_fields "${external_default_conflict_status_resp}" "external default drift status" \
  --equals peer.runtime_kind=external \
  --true running \
  --contains backend_policy_drift.changed_fields=default_runtime_kind \
  --equals backend_policy_drift.current_effective_start.runtime_kind=bundled \
  --equals backend_policy_drift.current_effective_start.default_runtime_kind_source=config \
  --true backend_policy_drift.current_effective_start.runtime_available

external_default_conflict_body="${RUN_LOG_DIR}/voice_webrtc_peer_external_default_conflict_body.json"
external_default_conflict_status="$(voice_peer_request_status "${external_default_conflict_body}" "{\"session_id\":\"${EXTERNAL_SESSION_ID}\",\"action\":\"start\"}")"
if [[ "${external_default_conflict_status}" != "409" ]]; then
  echo "expected effective default-runtime conflict to return 409, got ${external_default_conflict_status}" >&2
  cat "${external_default_conflict_body}" >&2
  exit 1
fi
voice_webrtc_assert_body_fields "${external_default_conflict_body}" "external default conflict response" \
  --equals error="voice peer already running with different config" \
  --equals peer.runtime_kind=external \
  --equals peer.tone_hz=901 \
  --contains backend_policy_drift.changed_fields=default_runtime_kind \
  --equals backend_policy_drift.current_effective_start.runtime_kind=bundled \
  --equals backend_policy_drift.current_effective_start.default_runtime_kind_source=config

external_unavailable_conflict_update_resp="$(config_update "$(python3 - <<PY
import json
print(json.dumps({
  "audio_webrtc": {
    "broker_url": "${VOICE_BROKER_URL}",
    "broker_token": "${VOICE_BROKER_TOKEN}",
    "peer_tool_path": None,
    "default_runtime_kind": "external",
    "node_bin": "node"
  }
}))
PY
)")"

voice_webrtc_assert_fields "${external_unavailable_conflict_update_resp}" "external unavailable conflict config" \
  --require-ok \
  --false audio_webrtc.peer_tool_path_configured \
  --equals audio_webrtc.default_runtime_kind=external \
  --equals audio_webrtc.default_runtime_kind_source=config \
  --false audio_webrtc.external_available \
  --false audio_webrtc.default_runtime_kind_available

external_unavailable_conflict_body="${RUN_LOG_DIR}/voice_webrtc_peer_external_unavailable_conflict_body.json"
external_unavailable_conflict_status="$(voice_peer_request_status "${external_unavailable_conflict_body}" "{\"session_id\":\"${EXTERNAL_SESSION_ID}\",\"action\":\"start\"}")"
if [[ "${external_unavailable_conflict_status}" != "409" ]]; then
  echo "expected unavailable-backend running conflict to return 409, got ${external_unavailable_conflict_status}" >&2
  cat "${external_unavailable_conflict_body}" >&2
  exit 1
fi
voice_webrtc_assert_body_fields "${external_unavailable_conflict_body}" "external unavailable conflict response" \
  --equals error="voice peer already running with different config" \
  --equals peer.runtime_kind=external \
  --equals "peer.tool_path=${PEER_TOOL}" \
  --contains backend_policy_drift.changed_fields=peer_tool_path \
  --equals backend_policy_drift.current_effective_start.runtime_kind=external \
  --false backend_policy_drift.current_effective_start.runtime_available \
  --contains backend_policy_drift.current_effective_start.runtime_unavailable_reason="not configured"

external_node_bin_conflict_update_resp="$(config_update "$(python3 - <<PY
import json
print(json.dumps({
  "audio_webrtc": {
    "broker_url": "${VOICE_BROKER_URL}",
    "broker_token": "${VOICE_BROKER_TOKEN}",
    "peer_tool_path": "${PEER_TOOL}",
    "default_runtime_kind": "external",
    "node_bin": "definitely-not-a-real-node-binary"
  }
}))
PY
)")"

voice_webrtc_assert_fields "${external_node_bin_conflict_update_resp}" "external node_bin conflict config" \
  --require-ok \
  --equals audio_webrtc.default_runtime_kind=external \
  --equals audio_webrtc.default_runtime_kind_source=config \
  --equals audio_webrtc.node_bin=definitely-not-a-real-node-binary \
  --false audio_webrtc.external_available \
  --false audio_webrtc.default_runtime_kind_available

external_node_bin_conflict_body="${RUN_LOG_DIR}/voice_webrtc_peer_external_node_bin_conflict_body.json"
external_node_bin_conflict_status="$(voice_peer_request_status "${external_node_bin_conflict_body}" "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${EXTERNAL_SESSION_ID}",
  "action": "start",
  "runtime_kind": "external",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-external-config",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 901
}))
PY
)")"
if [[ "${external_node_bin_conflict_status}" != "409" ]]; then
  echo "expected effective node_bin conflict to return 409, got ${external_node_bin_conflict_status}" >&2
  cat "${external_node_bin_conflict_body}" >&2
  exit 1
fi
voice_webrtc_assert_body_fields "${external_node_bin_conflict_body}" "external node_bin conflict response" \
  --equals error="voice peer already running with different config" \
  --equals peer.runtime_kind=external \
  --equals peer.node_bin=node \
  --contains backend_policy_drift.changed_fields=node_bin \
  --equals backend_policy_drift.current_effective_start.runtime_kind=external \
  --equals backend_policy_drift.current_effective_start.node_bin=definitely-not-a-real-node-binary \
  --false backend_policy_drift.current_effective_start.runtime_available \
  --contains backend_policy_drift.current_effective_start.runtime_unavailable_reason="not found"

external_restore_valid_config_resp="$(config_update "$(python3 - <<PY
import json
print(json.dumps({
  "audio_webrtc": {
    "broker_url": "${VOICE_BROKER_URL}",
    "broker_token": "${VOICE_BROKER_TOKEN}",
    "peer_tool_path": "${PEER_TOOL}",
    "default_runtime_kind": "external",
    "node_bin": "node"
  }
}))
PY
)")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${external_restore_valid_config_resp}''')
audio = obj.get("audio_webrtc") or {}
if not obj.get("ok"):
  print("failed to restore valid external config after running-conflict proofs", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("external_available") is not True or audio.get("default_runtime_kind_available") is not True:
  print("expected restored external availability after running-conflict proofs", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("node_bin") != "node" or audio.get("peer_tool_path_configured") is not True:
  print("expected restored node_bin/tool_path after running-conflict proofs", obj, file=sys.stderr)
  raise SystemExit(1)
PY

external_restored_status_resp="$(voice_peer_status "${EXTERNAL_SESSION_ID}")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${external_restored_status_resp}''')
if obj.get("running") is not True or (obj.get("peer") or {}).get("runtime_kind") != "external":
  print("expected running external peer after restoring valid config", obj, file=sys.stderr)
  raise SystemExit(1)
if "backend_policy_drift" in obj:
  print("did not expect backend_policy_drift after restoring valid external config", obj, file=sys.stderr)
  raise SystemExit(1)
PY

external_conflict_body="${RUN_LOG_DIR}/voice_webrtc_peer_external_conflict_body.json"
external_conflict_status="$(voice_peer_request_status "${external_conflict_body}" "{\"session_id\":\"${EXTERNAL_SESSION_ID}\",\"action\":\"start\",\"runtime_kind\":\"external\",\"tone_hz\":1234}")"
if [[ "${external_conflict_status}" != "409" ]]; then
  echo "expected external running conflict to return 409, got ${external_conflict_status}" >&2
  cat "${external_conflict_body}" >&2
  exit 1
fi
python3 - <<PY
import json, sys
obj = json.load(open(r'''${external_conflict_body}''', 'r', encoding='utf-8'))
peer = obj.get("peer") or {}
if obj.get("error") != "voice peer already running with different config":
  print("unexpected external running conflict response", obj, file=sys.stderr)
  raise SystemExit(1)
if peer.get("runtime_kind") != "external" or peer.get("tone_hz") != 901:
  print("unexpected external conflict peer snapshot", obj, file=sys.stderr)
  raise SystemExit(1)
if "backend_policy_drift" in obj:
  print("did not expect backend_policy_drift for explicit request conflict with restored policy", obj, file=sys.stderr)
  raise SystemExit(1)
PY

external_config_stop_resp="$(voice_peer_request "{\"session_id\":\"${EXTERNAL_SESSION_ID}\",\"action\":\"stop\",\"runtime_kind\":\"builtin\"}")"

voice_webrtc_assert_stopped "${external_config_stop_resp}" "external config-backed stop response" \
  --runtime-kind external \
  --stopped true \
  --broker-session-deleted true

wait_broker_session_deleted "${EXTERNAL_BROKER_SESSION_ID}"

invalid_broker_defaults_resp="$(config_update "$(python3 - <<PY
import json
print(json.dumps({
  "audio_webrtc": {
    "broker_url": "${VOICE_BROKER_URL}",
    "broker_token": "bad\\nvoice\\ntoken",
    "peer_tool_path": "${PEER_TOOL}",
    "default_runtime_kind": "external",
    "node_bin": "node"
  }
}))
PY
)")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${invalid_broker_defaults_resp}''')
if not obj.get("ok"):
  print("failed to set invalid broker token defaults for lazy-validation proof", obj, file=sys.stderr)
  raise SystemExit(1)
PY

MANAGED_BAD_STOP_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_managed_bad_stop_$(date +%s)_$RANDOM"
create_session "${MANAGED_BAD_STOP_SESSION_ID}"

managed_bad_stop_start_resp="$(voice_peer_request "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${MANAGED_BAD_STOP_SESSION_ID}",
  "action": "start",
  "runtime_kind": "external",
  "broker_url": "${VOICE_BROKER_URL}",
  "broker_token": "${VOICE_BROKER_TOKEN}",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-managed-bad-stop",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 779
}))
PY
)")"

voice_webrtc_assert_started "${managed_bad_stop_start_resp}" "managed bad-stop start response" \
  --runtime-kind external \
  --managed true

MANAGED_BAD_STOP_BROKER_SESSION_ID="$(voice_webrtc_peer_field "${managed_bad_stop_start_resp}" broker_session_id "managed bad-stop start response" --require-ok --nonempty)"

wait_voice_peer_ready "${MANAGED_BAD_STOP_SESSION_ID}" 1 status_json external external config 1

managed_bad_stop_resp="$(voice_peer_request "{\"session_id\":\"${MANAGED_BAD_STOP_SESSION_ID}\",\"action\":\"stop\"}")"

voice_webrtc_assert_stopped "${managed_bad_stop_resp}" "managed bad-stop response" \
  --stopped true \
  --broker-session-deleted false \
  --broker-session-delete-error-contains "invalid configured audio_webrtc_broker_token" \
  --managed true \
  --peer-running false

managed_bad_stop_broker_status="$(broker_session_status_code "${MANAGED_BAD_STOP_BROKER_SESSION_ID}")"
if [[ "${managed_bad_stop_broker_status}" != "200" && "${managed_bad_stop_broker_status}" != "404" ]]; then
  echo "expected managed bad-stop broker session inspect to return 200 or 404, got ${managed_bad_stop_broker_status}" >&2
  exit 1
fi

managed_bad_stop_delete_status="$(broker_session_delete_status "${MANAGED_BAD_STOP_BROKER_SESSION_ID}")"
if [[ "${managed_bad_stop_delete_status}" != "200" && "${managed_bad_stop_delete_status}" != "404" ]]; then
  echo "expected managed bad-stop broker session delete to return 200 or 404, got ${managed_bad_stop_delete_status}" >&2
  exit 1
fi

MANAGED_BAD_DELETE_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_managed_bad_delete_$(date +%s)_$RANDOM"
create_session "${MANAGED_BAD_DELETE_SESSION_ID}"

managed_bad_delete_start_resp="$(voice_peer_request "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${MANAGED_BAD_DELETE_SESSION_ID}",
  "action": "start",
  "runtime_kind": "external",
  "broker_url": "${VOICE_BROKER_URL}",
  "broker_token": "${VOICE_BROKER_TOKEN}",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-managed-bad-delete",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 780
}))
PY
)")"

voice_webrtc_assert_started "${managed_bad_delete_start_resp}" "managed bad-delete start response" \
  --runtime-kind external \
  --managed true

MANAGED_BAD_DELETE_BROKER_SESSION_ID="$(voice_webrtc_peer_field "${managed_bad_delete_start_resp}" broker_session_id "managed bad-delete start response" --require-ok --nonempty)"

wait_voice_peer_ready "${MANAGED_BAD_DELETE_SESSION_ID}" 1 status_json external external config 1

managed_bad_delete_resp="$(delete_session "${MANAGED_BAD_DELETE_SESSION_ID}")"

voice_webrtc_assert_fields "${managed_bad_delete_resp}" "managed bad-delete cleanup" \
  --require-ok \
  --true deleted_from_db \
  --true voice_runtime_cleanup.runtime_present \
  --true voice_runtime_cleanup.stopped \
  --true voice_runtime_cleanup.broker_session_delete_attempted \
  --false voice_runtime_cleanup.broker_session_deleted \
  --contains voice_runtime_cleanup.broker_session_delete_error="invalid configured audio_webrtc_broker_token" \
  --true voice_runtime_cleanup.peer.managed_broker_session

managed_bad_delete_broker_status="$(broker_session_status_code "${MANAGED_BAD_DELETE_BROKER_SESSION_ID}")"
if [[ "${managed_bad_delete_broker_status}" != "200" && "${managed_bad_delete_broker_status}" != "404" ]]; then
  echo "expected managed bad-delete broker session inspect to return 200 or 404, got ${managed_bad_delete_broker_status}" >&2
  exit 1
fi

managed_bad_delete_delete_status="$(broker_session_delete_status "${MANAGED_BAD_DELETE_BROKER_SESSION_ID}")"
if [[ "${managed_bad_delete_delete_status}" != "200" && "${managed_bad_delete_delete_status}" != "404" ]]; then
  echo "expected managed bad-delete broker session delete to return 200 or 404, got ${managed_bad_delete_delete_status}" >&2
  exit 1
fi

BORROWED_STOP_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_borrowed_stop_$(date +%s)_$RANDOM"
create_session "${BORROWED_STOP_SESSION_ID}"

borrowed_stop_broker_create_resp="$(broker_session_create '{"agent_id":"a-1","mode":"webrtc"}')"

BORROWED_STOP_BROKER_SESSION_ID="$(voice_webrtc_json_field "${borrowed_stop_broker_create_resp}" session_id "borrowed stop broker create response" --require-ok --nonempty)"

borrowed_stop_start_resp="$(voice_peer_request "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${BORROWED_STOP_SESSION_ID}",
  "action": "start",
  "runtime_kind": "external",
  "broker_session_id": "${BORROWED_STOP_BROKER_SESSION_ID}",
  "broker_url": "${VOICE_BROKER_URL}",
  "broker_token": "${VOICE_BROKER_TOKEN}",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 777
}))
PY
)")"

voice_webrtc_assert_started "${borrowed_stop_start_resp}" "borrowed stop start response" \
  --runtime-kind external \
  --managed false

wait_voice_peer_ready "${BORROWED_STOP_SESSION_ID}" 1 status_json external external config 1

borrowed_stop_resp="$(voice_peer_request "{\"session_id\":\"${BORROWED_STOP_SESSION_ID}\",\"action\":\"stop\"}")"

voice_webrtc_assert_stopped "${borrowed_stop_resp}" "borrowed stop response" \
  --stopped true \
  --broker-session-deleted-absent \
  --managed false

borrowed_stop_delete_status="$(broker_session_delete_status "${BORROWED_STOP_BROKER_SESSION_ID}")"
if [[ "${borrowed_stop_delete_status}" != "200" && "${borrowed_stop_delete_status}" != "404" ]]; then
  echo "expected borrowed stop broker session delete to return 200 or 404, got ${borrowed_stop_delete_status}" >&2
  exit 1
fi

BORROWED_DELETE_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_borrowed_delete_$(date +%s)_$RANDOM"
create_session "${BORROWED_DELETE_SESSION_ID}"

borrowed_delete_broker_create_resp="$(broker_session_create '{"agent_id":"a-1","mode":"webrtc"}')"

BORROWED_DELETE_BROKER_SESSION_ID="$(voice_webrtc_json_field "${borrowed_delete_broker_create_resp}" session_id "borrowed delete broker create response" --require-ok --nonempty)"

borrowed_delete_start_resp="$(voice_peer_request "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${BORROWED_DELETE_SESSION_ID}",
  "action": "start",
  "runtime_kind": "external",
  "broker_session_id": "${BORROWED_DELETE_BROKER_SESSION_ID}",
  "broker_url": "${VOICE_BROKER_URL}",
  "broker_token": "${VOICE_BROKER_TOKEN}",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 778
}))
PY
)")"

voice_webrtc_assert_started "${borrowed_delete_start_resp}" "borrowed delete start response" \
  --runtime-kind external \
  --managed false

wait_voice_peer_ready "${BORROWED_DELETE_SESSION_ID}" 1 status_json external external config 1

borrowed_delete_resp="$(delete_session "${BORROWED_DELETE_SESSION_ID}")"

voice_webrtc_assert_fields "${borrowed_delete_resp}" "borrowed delete cleanup" \
  --require-ok \
  --true voice_runtime_cleanup.runtime_present \
  --true voice_runtime_cleanup.stopped \
  --false voice_runtime_cleanup.broker_session_delete_attempted \
  --false voice_runtime_cleanup.peer.managed_broker_session

borrowed_delete_broker_delete_status="$(broker_session_delete_status "${BORROWED_DELETE_BROKER_SESSION_ID}")"
if [[ "${borrowed_delete_broker_delete_status}" != "200" && "${borrowed_delete_broker_delete_status}" != "404" ]]; then
  echo "expected borrowed delete broker session delete to return 200 or 404, got ${borrowed_delete_broker_delete_status}" >&2
  exit 1
fi

restore_broker_defaults_resp="$(config_update "$(python3 - <<PY
import json
print(json.dumps({
  "audio_webrtc": {
    "broker_url": "${VOICE_BROKER_URL}",
    "broker_token": "${VOICE_BROKER_TOKEN}",
    "peer_tool_path": "${PEER_TOOL}",
    "default_runtime_kind": "external",
    "node_bin": "node"
  }
}))
PY
)")"

python3 - <<PY
import json, sys
obj = json.loads(r'''${restore_broker_defaults_resp}''')
if not obj.get("ok"):
  print("failed to restore broker defaults after lazy-validation proof", obj, file=sys.stderr)
  raise SystemExit(1)
PY

NOOP_STOP_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_noop_stop_$(date +%s)_$RANDOM"
create_session "${NOOP_STOP_SESSION_ID}"

noop_stop_resp="$(voice_peer_request "{\"session_id\":\"${NOOP_STOP_SESSION_ID}\",\"action\":\"stop\",\"runtime_kind\":\"not-a-real-runtime-kind\"}")"

voice_webrtc_assert_stopped "${noop_stop_resp}" "noop stop response" \
  --stopped false \
  --reason not_running \
  --peer-absent

delete_session_quiet "${NOOP_STOP_SESSION_ID}"

delete_session_quiet "${EXTERNAL_SESSION_ID}"

unavailable_default_config_resp="$(config_update "$(python3 - <<PY
import json
print(json.dumps({
  "audio_webrtc": {
    "peer_tool_path": None,
    "default_runtime_kind": "external",
    "node_bin": "node"
  }
}))
PY
)")"

voice_webrtc_assert_fields "${unavailable_default_config_resp}" "unavailable external default config" \
  --require-ok \
  --false audio_webrtc.peer_tool_path_configured \
  --false audio_webrtc.builtin_available \
  --true audio_webrtc.bundled_available \
  --false audio_webrtc.external_available \
  --equals audio_webrtc.default_runtime_kind=external \
  --equals audio_webrtc.default_runtime_kind_source=config \
  --false audio_webrtc.default_runtime_kind_available \
  --equals audio_webrtc.node_bin=node

restart_agentd_without_voice_defaults

unavailable_default_config_get="$(config_get)"

voice_webrtc_assert_fields "${unavailable_default_config_get}" "persisted unavailable external default config" \
  --false daemon.audio_webrtc.peer_tool_path_configured \
  --false daemon.audio_webrtc.builtin_available \
  --true daemon.audio_webrtc.bundled_available \
  --false daemon.audio_webrtc.external_available \
  --equals daemon.audio_webrtc.default_runtime_kind=external \
  --equals daemon.audio_webrtc.default_runtime_kind_source=config \
  --false daemon.audio_webrtc.default_runtime_kind_available

UNAVAILABLE_DEFAULT_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_default_unavailable_$(date +%s)_$RANDOM"
create_session "${UNAVAILABLE_DEFAULT_SESSION_ID}"

unavailable_default_start_body="${RUN_LOG_DIR}/voice_webrtc_peer_unavailable_default_start_body.json"
unavailable_default_start_code="$(voice_peer_request_status "${unavailable_default_start_body}" "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${UNAVAILABLE_DEFAULT_SESSION_ID}",
  "action": "start",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-default-unavailable",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 903
}))
PY
)")"
if [[ "${unavailable_default_start_code}" != "500" ]]; then
  echo "expected unavailable external default start to return http 500, got ${unavailable_default_start_code}" >&2
  cat "${unavailable_default_start_body}" >&2
  exit 1
fi

voice_webrtc_assert_body_fields "${unavailable_default_start_body}" "unavailable external default start response" \
  --false ok \
  --false builtin_available \
  --true bundled_available \
  --false external_available \
  --equals default_runtime_kind=external \
  --equals default_runtime_kind_source=config \
  --false default_runtime_kind_available \
  --contains error="audio_webrtc_peer_tool_path not configured"

delete_session_quiet "${UNAVAILABLE_DEFAULT_SESSION_ID}"

builtin_default_config_resp="$(config_update "$(python3 - <<PY
import json
print(json.dumps({
  "audio_webrtc": {
    "peer_tool_path": "${PEER_TOOL}",
    "default_runtime_kind": "builtin",
    "node_bin": "node"
  }
}))
PY
)")"

voice_webrtc_assert_fields "${builtin_default_config_resp}" "builtin default config update" \
  --require-ok \
  --equals audio_webrtc.default_runtime_kind=builtin \
  --equals audio_webrtc.default_runtime_kind_source=config \
  --false audio_webrtc.default_runtime_kind_available \
  --false audio_webrtc.builtin_available \
  --true audio_webrtc.bundled_available \
  --true audio_webrtc.external_available \
  --contains audio_webrtc.builtin_unavailable_reason=disabled \
  --contains audio_webrtc.default_runtime_kind_unavailable_reason=disabled

restart_agentd_without_voice_defaults

builtin_default_config_get="$(config_get)"

voice_webrtc_assert_fields "${builtin_default_config_get}" "persisted builtin default config" \
  --equals daemon.audio_webrtc.default_runtime_kind=builtin \
  --equals daemon.audio_webrtc.default_runtime_kind_source=config \
  --false daemon.audio_webrtc.default_runtime_kind_available \
  --contains daemon.audio_webrtc.default_runtime_kind_unavailable_reason=disabled

BUILTIN_DEFAULT_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_default_builtin_$(date +%s)_$RANDOM"
create_session "${BUILTIN_DEFAULT_SESSION_ID}"

builtin_default_start_body="${RUN_LOG_DIR}/voice_webrtc_peer_builtin_default_start_body.json"
builtin_default_start_code="$(voice_peer_request_status "${builtin_default_start_body}" "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${BUILTIN_DEFAULT_SESSION_ID}",
  "action": "start",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-default-builtin",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 904
}))
PY
)")"
if [[ "${builtin_default_start_code}" != "501" ]]; then
  echo "expected config builtin default start to return http 501, got ${builtin_default_start_code}" >&2
  cat "${builtin_default_start_body}" >&2
  exit 1
fi

voice_webrtc_assert_body_fields "${builtin_default_start_body}" "builtin default start response" \
  --false ok \
  --equals default_runtime_kind=builtin \
  --equals default_runtime_kind_source=config \
  --false default_runtime_kind_available \
  --contains error=disabled

delete_session_quiet "${BUILTIN_DEFAULT_SESSION_ID}"

restored_external_default_config_resp="$(config_update "$(python3 - <<PY
import json
print(json.dumps({
  "audio_webrtc": {
    "peer_tool_path": "${PEER_TOOL}",
    "default_runtime_kind": "external",
    "node_bin": "node"
  }
}))
PY
)")"

voice_webrtc_assert_fields "${restored_external_default_config_resp}" "restored external default config" \
  --require-ok \
  --true audio_webrtc.peer_tool_path_configured \
  --true audio_webrtc.external_available \
  --true audio_webrtc.default_runtime_kind_available

python3 - <<PY
import json, sqlite3
db_path = r'''${SESSION_DB_PATH}'''
conn = sqlite3.connect(db_path)
try:
    row = conn.execute("SELECT value FROM meta WHERE key = 'daemon.runtime_config_json'").fetchone()
    if row is None:
        raise SystemExit("missing daemon.runtime_config_json before corruption test")
    raw = row[0]
    data = json.loads(raw)
    audio = data.get("audio_webrtc") or {}
    audio["default_runtime_kind"] = "not-a-real-runtime-kind"
    data["audio_webrtc"] = audio
    conn.execute("UPDATE meta SET value = ? WHERE key = 'daemon.runtime_config_json'", (json.dumps(data),))
    conn.commit()
finally:
    conn.close()
PY

restart_agentd_without_voice_defaults

invalid_default_self_healed="$(config_get)"

python3 - <<PY
import json, sqlite3, sys
obj = json.loads(r'''${invalid_default_self_healed}''')
daemon = obj.get("daemon") or {}
audio = daemon.get("audio_webrtc") or {}
if audio.get("default_runtime_kind") is not None or audio.get("default_runtime_kind_source") != "auto":
  print("expected invalid persisted default_runtime_kind to self-heal to auto", obj, file=sys.stderr)
  raise SystemExit(1)
if audio.get("peer_tool_path_configured") is not True:
  print("expected external seam to remain configured after self-heal", obj, file=sys.stderr)
  raise SystemExit(1)
db_path = r'''${SESSION_DB_PATH}'''
conn = sqlite3.connect(db_path)
try:
    row = conn.execute("SELECT value FROM meta WHERE key = 'daemon.runtime_config_json'").fetchone()
    if row is None:
        print("missing daemon.runtime_config_json after self-heal", file=sys.stderr)
        raise SystemExit(1)
    data = json.loads(row[0])
    audio_cfg = data.get("audio_webrtc") or {}
    if audio_cfg.get("default_runtime_kind", "__missing__") is not None:
        print("expected persisted invalid default_runtime_kind to be rewritten to null", data, file=sys.stderr)
        raise SystemExit(1)
finally:
    conn.close()
PY

SELF_HEAL_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_self_heal_$(date +%s)_$RANDOM"
create_session "${SELF_HEAL_SESSION_ID}"

self_heal_start_resp="$(voice_peer_request "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${SELF_HEAL_SESSION_ID}",
  "action": "start",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-self-heal",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 902
}))
PY
)")"

voice_webrtc_assert_started "${self_heal_start_resp}" "self-heal fallback start response" \
  --runtime-kind bundled \
  --default-runtime-kind bundled \
  --default-runtime-kind-source auto

SELF_HEAL_BROKER_SESSION_ID="$(voice_webrtc_peer_field "${self_heal_start_resp}" broker_session_id "self-heal start response" --require-ok --nonempty)"

wait_voice_peer_ready "${SELF_HEAL_SESSION_ID}" 1 status_json bundled bundled auto 1

self_heal_stop_resp="$(voice_peer_request "{\"session_id\":\"${SELF_HEAL_SESSION_ID}\",\"action\":\"stop\"}")"

voice_webrtc_assert_stopped "${self_heal_stop_resp}" "self-heal fallback stop response" \
  --stopped true \
  --broker-session-deleted true

wait_broker_session_deleted "${SELF_HEAL_BROKER_SESSION_ID}"

delete_session_quiet "${SELF_HEAL_SESSION_ID}"

CORRUPT_RUNTIME_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_corrupt_record_$(date +%s)_$RANDOM"
create_session "${CORRUPT_RUNTIME_SESSION_ID}"

CORRUPT_RUNTIME_DIR="${STATE_DIR}/voice_webrtc_peers/${CORRUPT_RUNTIME_SESSION_ID}"
python3 "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_record.py" write-corrupt \
  --db-path "${SESSION_DB_PATH}" \
  --session-id "${CORRUPT_RUNTIME_SESSION_ID}" \
  --value "{ definitely-not-json" \
  --runtime-dir "${CORRUPT_RUNTIME_DIR}" \
  --artifact-json '{"stale":"artifact"}'

corrupt_runtime_status="$(voice_peer_status "${CORRUPT_RUNTIME_SESSION_ID}")"

voice_webrtc_assert_runtime_cleared "${corrupt_runtime_status}" "${CORRUPT_RUNTIME_SESSION_ID}" "${CORRUPT_RUNTIME_DIR}" cleanup_on_corrupt_record "corrupt runtime status" \
  --expect-session-exists true --expect-running false

python3 "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_record.py" write-corrupt \
  --db-path "${SESSION_DB_PATH}" \
  --session-id "${CORRUPT_RUNTIME_SESSION_ID}" \
  --value "{ definitely-not-json-again"

corrupt_runtime_start_resp="$(voice_peer_request "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${CORRUPT_RUNTIME_SESSION_ID}",
  "action": "start",
  "runtime_kind": "bundled",
  "broker_url": "${VOICE_BROKER_URL}",
  "broker_token": "${VOICE_BROKER_TOKEN}",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-corrupt-runtime-self-heal",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 903
}))
PY
)")"

voice_webrtc_assert_fields "${corrupt_runtime_start_resp}" "corrupt runtime self-heal start response" \
  --true ok \
  --true started \
  --true cleanup_on_corrupt_record.persisted_record_cleared \
  --equals peer.runtime_kind=bundled

CORRUPT_RUNTIME_BROKER_SESSION_ID="$(voice_webrtc_peer_field "${corrupt_runtime_start_resp}" broker_session_id "corrupt runtime self-heal start response" --require-ok --nonempty)"

wait_voice_peer_ready "${CORRUPT_RUNTIME_SESSION_ID}" 1 status_json bundled bundled auto 1

PLANNED_RUNTIME_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_planned_record_$(date +%s)_$RANDOM"
create_session "${PLANNED_RUNTIME_SESSION_ID}"

PLANNED_RUNTIME_DIR="${STATE_DIR}/voice_webrtc_peers/${PLANNED_RUNTIME_SESSION_ID}"
python3 "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_record.py" write-planned \
  --db-path "${SESSION_DB_PATH}" \
  --runtime-dir "${PLANNED_RUNTIME_DIR}" \
  --session-id "${PLANNED_RUNTIME_SESSION_ID}" \
  --broker-url "${VOICE_BROKER_URL}"

planned_runtime_status="$(voice_peer_status "${PLANNED_RUNTIME_SESSION_ID}")"

voice_webrtc_assert_runtime_cleared "${planned_runtime_status}" "${PLANNED_RUNTIME_SESSION_ID}" "${PLANNED_RUNTIME_DIR}" cleanup_on_corrupt_record "planned runtime status" \
  --expect-session-exists true --expect-running false

corrupt_runtime_stop_resp="$(voice_peer_request "{\"session_id\":\"${CORRUPT_RUNTIME_SESSION_ID}\",\"action\":\"stop\"}")"

voice_webrtc_assert_stopped "${corrupt_runtime_stop_resp}" "corrupt runtime self-heal stop response" \
  --stopped true \
  --broker-session-deleted true

wait_broker_session_deleted "${CORRUPT_RUNTIME_BROKER_SESSION_ID}"

delete_session_quiet "${CORRUPT_RUNTIME_SESSION_ID}"

STALE_STATUS_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_stale_status_$(date +%s)_$RANDOM"
create_session "${STALE_STATUS_SESSION_ID}"
inject_stale_persisted_voice_runtime "${STALE_STATUS_SESSION_ID}"
STALE_STATUS_RUNTIME_DIR="${STATE_DIR}/voice_webrtc_peers/${STALE_STATUS_SESSION_ID}"
stale_status_resp="$(voice_peer_status "${STALE_STATUS_SESSION_ID}")"

voice_webrtc_assert_runtime_cleared "${stale_status_resp}" "${STALE_STATUS_SESSION_ID}" "${STALE_STATUS_RUNTIME_DIR}" cleanup_on_stale_record "stale runtime status" \
  --expect-running false

delete_session_quiet "${STALE_STATUS_SESSION_ID}"

STALE_STOP_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_stale_stop_$(date +%s)_$RANDOM"
create_session "${STALE_STOP_SESSION_ID}"
inject_stale_persisted_voice_runtime "${STALE_STOP_SESSION_ID}"
STALE_STOP_RUNTIME_DIR="${STATE_DIR}/voice_webrtc_peers/${STALE_STOP_SESSION_ID}"
stale_stop_resp="$(voice_peer_request "{\"session_id\":\"${STALE_STOP_SESSION_ID}\",\"action\":\"stop\"}")"

voice_webrtc_assert_runtime_cleared "${stale_stop_resp}" "${STALE_STOP_SESSION_ID}" "${STALE_STOP_RUNTIME_DIR}" cleanup_on_stale_record "stale runtime stop" \
  --expect-stopped false --expect-reason not_running

delete_session_quiet "${STALE_STOP_SESSION_ID}"

STALE_START_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_stale_start_$(date +%s)_$RANDOM"
create_session "${STALE_START_SESSION_ID}"
inject_stale_persisted_voice_runtime "${STALE_START_SESSION_ID}"
STALE_START_RUNTIME_DIR="${STATE_DIR}/voice_webrtc_peers/${STALE_START_SESSION_ID}"

stale_start_resp="$(voice_peer_request "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${STALE_START_SESSION_ID}",
  "action": "start",
  "runtime_kind": "bundled",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-stale-runtime-self-heal",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 911
}))
PY
)")"

voice_webrtc_assert_fields "${stale_start_resp}" "stale runtime self-heal start response" \
  --true ok \
  --true started \
  --true cleanup_on_stale_record.persisted_record_cleared \
  --true cleanup_on_stale_record.runtime_artifacts_deleted \
  --equals peer.runtime_kind=bundled \
  --true peer.running
if [[ -f "${STALE_START_RUNTIME_DIR}/stdout.jsonl" ]] && grep -Fq '{"stale":"artifact"}' "${STALE_START_RUNTIME_DIR}/stdout.jsonl"; then
  echo "expected stale stdout artifact to be replaced on fresh start" >&2
  exit 1
fi

STALE_START_BROKER_SESSION_ID="$(voice_webrtc_peer_field "${stale_start_resp}" broker_session_id "stale runtime self-heal start response" --require-ok --nonempty)"

wait_voice_peer_ready "${STALE_START_SESSION_ID}" 1 status_json bundled bundled auto 1

stale_start_stop_resp="$(voice_peer_request "{\"session_id\":\"${STALE_START_SESSION_ID}\",\"action\":\"stop\"}")"

voice_webrtc_assert_stopped "${stale_start_stop_resp}" "stale runtime self-heal stop response" \
  --stopped true \
  --broker-session-deleted true

wait_broker_session_deleted "${STALE_START_BROKER_SESSION_ID}"

delete_session_quiet "${STALE_START_SESSION_ID}"

BUILTIN_STALE_STATUS_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_builtin_stale_status_$(date +%s)_$RANDOM"
create_session "${BUILTIN_STALE_STATUS_SESSION_ID}"
inject_stale_persisted_voice_runtime "${BUILTIN_STALE_STATUS_SESSION_ID}" builtin
BUILTIN_STALE_STATUS_RUNTIME_DIR="${STATE_DIR}/voice_webrtc_peers/${BUILTIN_STALE_STATUS_SESSION_ID}"
builtin_stale_status_resp="$(voice_peer_status "${BUILTIN_STALE_STATUS_SESSION_ID}")"

voice_webrtc_assert_runtime_cleared "${builtin_stale_status_resp}" "${BUILTIN_STALE_STATUS_SESSION_ID}" "${BUILTIN_STALE_STATUS_RUNTIME_DIR}" cleanup_on_stale_record "builtin stale runtime status" \
  --expect-running false

delete_session_quiet "${BUILTIN_STALE_STATUS_SESSION_ID}"

BUILTIN_STALE_STOP_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_builtin_stale_stop_$(date +%s)_$RANDOM"
create_session "${BUILTIN_STALE_STOP_SESSION_ID}"
inject_stale_persisted_voice_runtime "${BUILTIN_STALE_STOP_SESSION_ID}" builtin
BUILTIN_STALE_STOP_RUNTIME_DIR="${STATE_DIR}/voice_webrtc_peers/${BUILTIN_STALE_STOP_SESSION_ID}"
builtin_stale_stop_resp="$(voice_peer_request "{\"session_id\":\"${BUILTIN_STALE_STOP_SESSION_ID}\",\"action\":\"stop\"}")"

voice_webrtc_assert_runtime_cleared "${builtin_stale_stop_resp}" "${BUILTIN_STALE_STOP_SESSION_ID}" "${BUILTIN_STALE_STOP_RUNTIME_DIR}" cleanup_on_stale_record "builtin stale runtime stop" \
  --expect-stopped false --expect-reason not_running

delete_session_quiet "${BUILTIN_STALE_STOP_SESSION_ID}"

BUILTIN_STALE_START_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_builtin_stale_start_$(date +%s)_$RANDOM"
create_session "${BUILTIN_STALE_START_SESSION_ID}"
inject_stale_persisted_voice_runtime "${BUILTIN_STALE_START_SESSION_ID}" builtin
BUILTIN_STALE_START_RUNTIME_DIR="${STATE_DIR}/voice_webrtc_peers/${BUILTIN_STALE_START_SESSION_ID}"

builtin_stale_start_resp="$(voice_peer_request "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${BUILTIN_STALE_START_SESSION_ID}",
  "action": "start",
  "runtime_kind": "bundled",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-builtin-stale-runtime-self-heal",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 913
}))
PY
)")"

voice_webrtc_assert_fields "${builtin_stale_start_resp}" "builtin stale runtime self-heal start response" \
  --true ok \
  --true started \
  --true cleanup_on_stale_record.persisted_record_cleared \
  --true cleanup_on_stale_record.runtime_artifacts_deleted \
  --equals peer.runtime_kind=bundled \
  --true peer.running
if [[ -f "${BUILTIN_STALE_START_RUNTIME_DIR}/stdout.jsonl" ]] && grep -Fq '{"stale":"artifact"}' "${BUILTIN_STALE_START_RUNTIME_DIR}/stdout.jsonl"; then
  echo "expected builtin stale stdout artifact to be replaced on fresh start" >&2
  exit 1
fi

BUILTIN_STALE_START_BROKER_SESSION_ID="$(voice_webrtc_peer_field "${builtin_stale_start_resp}" broker_session_id "builtin stale runtime self-heal start response" --require-ok --nonempty)"

wait_voice_peer_ready "${BUILTIN_STALE_START_SESSION_ID}" 1 status_json bundled bundled auto 1

builtin_stale_start_stop_resp="$(voice_peer_request "{\"session_id\":\"${BUILTIN_STALE_START_SESSION_ID}\",\"action\":\"stop\"}")"

voice_webrtc_assert_stopped "${builtin_stale_start_stop_resp}" "builtin stale runtime self-heal stop response" \
  --stopped true \
  --broker-session-deleted true

wait_broker_session_deleted "${BUILTIN_STALE_START_BROKER_SESSION_ID}"

delete_session_quiet "${BUILTIN_STALE_START_SESSION_ID}"

config_invalid_node_bin_update_resp="$(config_update "$(python3 - <<PY
import json
print(json.dumps({
  "audio_webrtc": {
    "node_bin": "definitely-not-a-real-node-binary"
  }
}))
PY
)")"

voice_webrtc_assert_fields "${config_invalid_node_bin_update_resp}" "invalid node_bin config update" \
  --require-ok \
  --equals audio_webrtc.node_bin=definitely-not-a-real-node-binary \
  --false audio_webrtc.bundled_available \
  --false audio_webrtc.external_available \
  --false audio_webrtc.default_runtime_kind_available \
  --contains audio_webrtc.bundled_unavailable_reason="not found" \
  --contains audio_webrtc.external_unavailable_reason="not found"

restart_agentd_without_voice_defaults

invalid_node_bin_config_json="$(config_get)"

voice_webrtc_assert_fields "${invalid_node_bin_config_json}" "persisted invalid node_bin config" \
  --none daemon.audio_webrtc.default_runtime_kind \
  --equals daemon.audio_webrtc.default_runtime_kind_source=auto \
  --false daemon.audio_webrtc.bundled_available \
  --false daemon.audio_webrtc.external_available \
  --false daemon.audio_webrtc.default_runtime_kind_available \
  --contains daemon.audio_webrtc.default_runtime_kind_unavailable_reason="not found"

INVALID_NODE_BIN_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_invalid_node_bin_$(date +%s)_$RANDOM"
create_session "${INVALID_NODE_BIN_SESSION_ID}"

invalid_node_bin_start_body="${RUN_LOG_DIR}/voice_webrtc_peer_invalid_node_bin_start_body.json"
invalid_node_bin_start_status="$(voice_peer_request_status "${invalid_node_bin_start_body}" "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${INVALID_NODE_BIN_SESSION_ID}",
  "action": "start",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-invalid-node-bin",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 932,
  "startup_wait_ms": 1000
}))
PY
)")"
if [[ "${invalid_node_bin_start_status}" != "500" ]]; then
  echo "expected invalid node_bin start response to return 500, got ${invalid_node_bin_start_status}" >&2
  cat "${invalid_node_bin_start_body}" >&2
  exit 1
fi

voice_webrtc_assert_body_fields "${invalid_node_bin_start_body}" "invalid node_bin start response" \
  --false default_runtime_kind_available \
  --contains error="audio_webrtc_peer_node_bin not found" \
  --none startup_cleanup \
  --none peer \
  --contains default_runtime_kind_unavailable_reason="not found"

invalid_node_bin_status_json="$(voice_peer_status "${INVALID_NODE_BIN_SESSION_ID}")"

python3 "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_assertions.py" assert-no-runtime \
  --label "invalid node_bin preflight failure" \
  --response-json "${invalid_node_bin_status_json}" \
  --expect-session-exists true

delete_session_quiet "${INVALID_NODE_BIN_SESSION_ID}"

config_failfast_update_resp="$(config_update "$(python3 - <<PY
import json
print(json.dumps({
  "audio_webrtc": {
    "node_bin": "false"
  }
}))
PY
)")"

voice_webrtc_assert_fields "${config_failfast_update_resp}" "fail-fast node_bin config" \
  --require-ok \
  --equals-string audio_webrtc.node_bin=false \
  --true audio_webrtc.bundled_available \
  --true audio_webrtc.default_runtime_kind_available

restart_agentd_without_voice_defaults

FAIL_SESSION_ID="agentd_session_voice_webrtc_peer_runtime_fail_$(date +%s)_$RANDOM"
create_session "${FAIL_SESSION_ID}"

fail_start_body="${RUN_LOG_DIR}/voice_webrtc_peer_fail_start_body.json"
fail_start_status="$(voice_peer_request_status "${fail_start_body}" "$(python3 - <<PY
import json
print(json.dumps({
  "session_id": "${FAIL_SESSION_ID}",
  "action": "start",
  "broker_agent_id": "a-1",
  "broker_deployment_id": "lab-fail-fast",
  "sender_tag": "agentd_runtime_peer",
  "deadline_ms": 15000,
  "poll_interval_ms": 100,
  "tone_hz": 932,
  "startup_wait_ms": 1000
}))
PY
)")"
if [[ "${fail_start_status}" != "500" ]]; then
  echo "expected fail-fast startup response to return 500, got ${fail_start_status}" >&2
  cat "${fail_start_body}" >&2
  exit 1
fi

voice_webrtc_assert_body_fields "${fail_start_body}" "fail-fast startup response" \
  --false startup_confirmed \
  --true startup_cleanup.runtime_present \
  --true startup_cleanup.broker_session_delete_attempted \
  --true startup_cleanup.broker_session_deleted \
  --equals peer.exit_code=1 \
  --contains error="exited before ready"

fail_status_json="$(voice_peer_status "${FAIL_SESSION_ID}")"

python3 "${SCRIPT_DIR}/lib/agentd_voice_webrtc_runtime_assertions.py" assert-no-runtime \
  --label "fail-fast startup cleanup" \
  --response-json "${fail_status_json}" \
  --expect-session-exists true

delete_session_quiet "${FAIL_SESSION_ID}"

echo "agentd_session_voice_webrtc_peer_runtime_smoke OK"
